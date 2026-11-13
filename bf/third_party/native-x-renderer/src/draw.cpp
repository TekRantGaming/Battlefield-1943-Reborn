// NativeXRenderer - see draw.h.

#include "draw.h"

#include <native_x_renderer/native_x_renderer.h>

#include "blit.h"

#include "device.h"
#include "glsl.h"
#include "guest_memory.h"
#include "guest_textures.h"
#include "pipelines.h"
#include "primitives.h"
#include "shaders.h"
#include "state.h"
#include "storage.h"
#include "targets.h"
#include "textures.h"

#include <rex/bit.h>
#include <rex/types.h>
#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdlib>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <xxhash.h>

REXCVAR_DEFINE_INT32(native_x_renderer_draw_log, 0, "NativeXRenderer",
                     "Bring-up: log the first N draws NativeXRenderer processes (primitive, indices, shaders)");
REXCVAR_DEFINE_STRING(native_x_renderer_alpha_to_coverage, "auto", "NativeXRenderer",
                      "Alpha to coverage on targets without MSAA (D69, D114): auto (blend on surfaces the game draws "
                      "with MSAA, dither on those it draws at 1x), blend (the color blended by its alpha, depth written "
                      "where the alpha is at least 0.5: close to the game's MSAA) or dither (the shader's 2x2 pattern, "
                      "as on the console without MSAA)");

REXCVAR_DEFINE_BOOL(native_x_renderer_tick_interpolation, false, "NativeXRenderer",
                    "Objects that move in the game's 30 Hz tick drawn between their last two ticks' positions, by the "
                    "time since the last one (D138): in a vehicle, where the camera moves only in the tick, the vehicle "
                    "and the world move smoothly above 30 FPS, one tick behind. Off: a draw that cannot be matched is drawn "
                    "with the tick's camera while its neighbours are blended, and gaps show (run vcam5)");

namespace native_x_renderer {

using namespace Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
using Translator = rg::SpirvShaderTranslator;

// What one draw computed, for the system constants.
struct DrawContext {
  const rg::RegisterFile* regs;
  const rg::PrimitiveProcessor::ProcessingResult* primitives;
  const draw_util::ViewportInfo* viewport;
  bool polygonal;
  uint32_t used_texture_mask;
  bool alpha_to_coverage_blended;  // D69: no dither; a depth-writing draw tests alpha against 0.5
  bool depth_write;
};

namespace {

// Registers one draw changes for itself, restored when it returns.
class ScopedRegisters {
 public:
  explicit ScopedRegisters(rg::RegisterFile& registers) : registers_(registers) {}
  ~ScopedRegisters() {
    for (uint32_t i = count_; i-- > 0;) registers_[saved_[i].first] = saved_[i].second;
  }
  void Set(uint32_t index, uint32_t value) {
    saved_[count_++] = {index, registers_[index]};
    registers_[index] = value;
  }

 private:
  rg::RegisterFile& registers_;
  std::array<std::pair<uint32_t, uint32_t>, 8> saved_{};
  uint32_t count_ = 0;
};

// Xenos blend factors to DiligentCore, as the SDK's D3D12 backend maps them: there are no constant alpha
// factors (the constant color is used), and the alpha channel takes the alpha version of color factors.
BLEND_FACTOR BlendFactor(xenos::BlendFactor factor, bool alpha) {
  switch (uint32_t(factor)) {
    case 1: return BLEND_FACTOR_ONE;
    case 4: return alpha ? BLEND_FACTOR_SRC_ALPHA : BLEND_FACTOR_SRC_COLOR;
    case 5: return alpha ? BLEND_FACTOR_INV_SRC_ALPHA : BLEND_FACTOR_INV_SRC_COLOR;
    case 6: return BLEND_FACTOR_SRC_ALPHA;
    case 7: return BLEND_FACTOR_INV_SRC_ALPHA;
    case 8: return alpha ? BLEND_FACTOR_DEST_ALPHA : BLEND_FACTOR_DEST_COLOR;
    case 9: return alpha ? BLEND_FACTOR_INV_DEST_ALPHA : BLEND_FACTOR_INV_DEST_COLOR;
    case 10: return BLEND_FACTOR_DEST_ALPHA;
    case 11: return BLEND_FACTOR_INV_DEST_ALPHA;
    case 12: case 14: return BLEND_FACTOR_BLEND_FACTOR;
    case 13: case 15: return BLEND_FACTOR_INV_BLEND_FACTOR;
    case 16: return BLEND_FACTOR_SRC_ALPHA_SAT;
    default: return BLEND_FACTOR_ZERO;
  }
}

BLEND_OPERATION BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract: return BLEND_OPERATION_SUBTRACT;
    case xenos::BlendOp::kMin: return BLEND_OPERATION_MIN;
    case xenos::BlendOp::kMax: return BLEND_OPERATION_MAX;
    case xenos::BlendOp::kRevSubtract: return BLEND_OPERATION_REV_SUBTRACT;
    default: return BLEND_OPERATION_ADD;
  }
}

// Xenos compare functions and stencil operations are DiligentCore's, minus one.
uint8_t Compare(xenos::CompareFunction function) { return uint8_t(uint32_t(function) + 1); }
uint8_t Stencil(xenos::StencilOp op) { return uint8_t(uint32_t(op) + 1); }

// The longest a draw into an offscreen target waits for its translations and pipeline before it is skipped after all
// (D101): a pipeline the driver has never seen takes up to a few hundred milliseconds.
constexpr std::chrono::milliseconds kPipelineWaitLimit{3000};

constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * 4;
constexpr uint32_t kFetchConstantsSize = 32 * 6 * 4;

RefCntAutoPtr<IBuffer> CreateConstantBuffer(IRenderDevice* device, const char* name, uint32_t size) {
  BufferDesc desc;
  desc.Name = name;
  desc.Size = size;
  desc.Usage = USAGE_DYNAMIC;
  desc.BindFlags = BIND_UNIFORM_BUFFER;
  desc.CPUAccessFlags = CPU_ACCESS_WRITE;
  RefCntAutoPtr<IBuffer> buffer;
  device->CreateBuffer(desc, nullptr, &buffer);
  return buffer;
}

// A dynamic buffer mapped for writing (new memory each time) until the end of the scope.
class MappedBuffer {
 public:
  MappedBuffer(IDeviceContext* context, IBuffer* buffer) : context_(context), buffer_(buffer) {
    context->MapBuffer(buffer, MAP_WRITE, MAP_FLAG_DISCARD, data_);
  }
  ~MappedBuffer() {
    if (data_) context_->UnmapBuffer(buffer_, MAP_WRITE);
  }
  uint8_t* data() const { return static_cast<uint8_t*>(data_); }

 private:
  IDeviceContext* context_;
  IBuffer* buffer_;
  void* data_ = nullptr;
};

// The motion pixel shader (D48): the two positions the motion vertex shader writes, in clip space, to host
// pixels with the viewport's scale (NDC Y up, pixels Y down), from this frame's position to the last frame's.
RefCntAutoPtr<IShader> CreateMotionPixelShader(IRenderDevice* device) {
  std::string source =
      "#version 450\n"
      "layout(set = 0, binding = 0) uniform XeMotionConstants {\n"
      "  vec4 ndc_to_pixels;  // half the viewport's width, minus half its height\n"
      "};\n"
      "layout(location = " + std::to_string(ShaderCache::kMotionPreviousLocation) + ") in vec4 previous_position;\n"
      "layout(location = " + std::to_string(ShaderCache::kMotionCurrentLocation) + ") in vec4 current_position;\n"
      "layout(location = 0) out vec2 out_motion;\n"
      "void main() {\n"
      "  out_motion = (previous_position.xy / previous_position.w - current_position.xy / current_position.w) *\n"
      "               ndc_to_pixels.xy;\n"
      "}\n";
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv = CompileGlsl(GlslStage::kPixel, source, "NativeXRenderer motion PS");
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = "NativeXRenderer motion PS";
  info.Desc.ShaderType = SHADER_TYPE_PIXEL;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

// Whether every texture a pipeline's variables hold can be sampled in the state it is in (D82): what
// CommitShaderResources would otherwise transition, a texture loaded, copied or drawn into since its last commit.
// The test is DiligentCore's (ShaderResourceCacheVk::TransitionResources): depth textures are read as depth.
bool TextureReadable(ITextureView* view) {
  ITexture* texture = view->GetTexture();
  RESOURCE_STATE state = texture->GetState();
  RESOURCE_STATE required = (texture->GetDesc().BindFlags & BIND_DEPTH_STENCIL) ? RESOURCE_STATE_DEPTH_READ
                                                                                 : RESOURCE_STATE_SHADER_RESOURCE;
  return state == RESOURCE_STATE_UNKNOWN || (state & required) == required;
}

bool TexturesReadable(const Pipeline& pipeline) {
  for (const auto& objects : pipeline.texture_objects) {
    for (IDeviceObject* object : objects) {
      if (object && !TextureReadable(static_cast<ITextureView*>(object))) return false;
    }
  }
  return true;
}

// The same for a binding set's textures (D111).
bool TexturesReadable(const BindingSet& set) {
  for (ITextureView* view : set.views) {
    if (!TextureReadable(view)) return false;
  }
  return true;
}

// A warning logged the first time only (per call site).
#define NATIVE_X_RENDERER_WARN_ONCE(...)   do {                                  static bool logged = false;         if (!logged) {                        logged = true;                      REXLOG_WARN(__VA_ARGS__);         }                                 } while (false)

// A draw whose reprojection is the camera's within this many pixels of the game's frame moved with the camera only
// (D120): the camera's motion is its own, and it needs no motion draw. During the frame the vote's leader stands for
// the camera once this many draws agree on it and they are half of the offers or more.
constexpr double kCameraMotionPx = 0.1;
constexpr uint32_t kLeaderVotes = 8;

}  // namespace

// A main view draw that moved, kept for DrawObjectMotion (D48).
struct DrawPath::MotionDraw {
  Pipeline* pipeline;
  // In motion_data_: the vertex shader's float constants of this frame, then the system, bool/loop and fetch
  // constants. Those of the last frame are ObjectMotion's own, kept until the frame ends (after DrawObjectMotion):
  // not copied again (D143, 4 KB a draw for most of Guadalcanal's).
  size_t data;
  uint32_t constants_size;
  Viewport viewport;
  Rect scissor;
  const uint8_t* previous_constants;
  IBuffer* index_buffer;  // null: not indexed
  uint64_t index_offset;
  VALUE_TYPE index_type;
  uint32_t vertex_count;
  // Its own reprojection from its perspective matrix (D120), when it has one: left to the camera's motion if that is
  // the camera vote's winner.
  bool has_reprojection;
  Mat4d reprojection;
};

DrawPath::DrawPath() = default;
DrawPath::~DrawPath() {
  if (save_thread_.joinable()) save_thread_.join();
  if (resolved_) resolved_->Watch(nullptr);  // before the guest memory mirror goes
}

bool DrawPath::Initialize(Device& device, rg::RegisterFile& registers, rex::memory::Memory& memory,
                          ResolvedTextures& resolved, const RenderScale& scale,
                          const std::filesystem::path& storage) {
  device_ = &device;
  registers_ = &registers;
  scale_ = scale;
  IRenderDevice* render_device = device.device();
  IDeviceContext* context = device.context();

  memory_ = std::make_unique<GuestMemory>(memory, render_device, context);
  if (!memory_->Initialize()) return false;
  // Resolved textures are invalidated when their guest memory changes, which the mirror's watches report (D70).
  resolved_ = &resolved;
  resolved.Watch(memory_.get());
  primitives_ = std::make_unique<Primitives>(registers, memory, *memory_, render_device, context);
  if (!primitives_->Initialize()) {
    REXLOG_ERROR("[native_x_renderer] could not initialize primitive processing");
    return false;
  }
  shaders_ = std::make_unique<ShaderCache>();
  if (!shaders_->Initialize(render_device, scale_)) return false;
  guest_textures_ = std::make_unique<GuestTextures>(registers, *memory_, resolved, render_device, context);
  if (!guest_textures_->Initialize()) return false;
  // Frame numbers are the device's: frame N is recorded after N - 1 presents and complete when the device's
  // frame fence reaches N.
  guest_textures_->BeginFrame(device.frames_presented() + 1, device.frames_completed());
  textures_ = std::make_unique<Textures>();
  if (!textures_->Initialize(render_device, resolved, *guest_textures_)) return false;

  system_constants_buffer_ =
      CreateConstantBuffer(render_device, "NativeXRenderer system constants", sizeof(Translator::SystemConstants));
  bool_loop_constants_buffer_ =
      CreateConstantBuffer(render_device, "NativeXRenderer bool/loop constants", kBoolLoopConstantsSize);
  fetch_constants_buffer_ = CreateConstantBuffer(render_device, "NativeXRenderer fetch constants", kFetchConstantsSize);
  motion_constants_buffer_ = CreateConstantBuffer(render_device, "NativeXRenderer motion constants", 16);
  if (!system_constants_buffer_ || !bool_loop_constants_buffer_ || !fetch_constants_buffer_ ||
      !motion_constants_buffer_) {
    return false;
  }

  PipelineBuffers buffers;
  buffers.shared_memory = memory_->view();
  buffers.system_constants = system_constants_buffer_;
  buffers.bool_loop_constants = bool_loop_constants_buffer_;
  buffers.fetch_constants = fetch_constants_buffer_;
  buffers.motion_constants = motion_constants_buffer_;
  pipelines_ = std::make_unique<PipelineCache>();
  pipelines_->Initialize(render_device, buffers);
  if (!storage.empty()) OpenStorage(storage);
  if (!storage_) SetStoredPipelinesFinished();  // nothing stored to wait for (D134)
  const std::string& alpha_to_coverage = REXCVAR_GET(native_x_renderer_alpha_to_coverage);
  tick_interpolation_ = REXCVAR_GET(native_x_renderer_tick_interpolation);  // D138
  alpha_to_coverage_blend_.msaa = alpha_to_coverage != "dither";
  alpha_to_coverage_blend_.one = alpha_to_coverage == "blend";
  REXLOG_INFO("[native_x_renderer] alpha to coverage: {}",
              alpha_to_coverage_blend_.one    ? "blended by alpha, depth where alpha >= 0.5 (D69)"
              : alpha_to_coverage_blend_.msaa ? "blended by alpha on surfaces with MSAA in the game (D69), the shader's "
                                                "2x2 dither at 1x as on the console (D114)"
                                              : "the shader's 2x2 dither, as at 1x on the console");
  return true;
}

void DrawPath::OpenStorage(const std::filesystem::path& storage) {
  IRenderDevice* render_device = device_->device();
  // DiligentCore's pipeline cache: the driver's binaries of the pipelines compiled before. Vulkan ignores
  // data from another device or driver.
  pipeline_cache_path_ = storage;
  pipeline_cache_path_ += ".vkcache";
  std::vector<uint8_t> blob;
  {
    std::ifstream file(pipeline_cache_path_, std::ios::binary);
    if (file) blob.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  PipelineStateCacheCreateInfo cache_info;
  cache_info.Desc.Name = "NativeXRenderer pipeline cache";
  cache_info.Desc.Mode = PSO_CACHE_MODE_LOAD_STORE;
  cache_info.pCacheData = blob.empty() ? nullptr : blob.data();
  cache_info.CacheDataSize = uint32_t(blob.size());
  render_device->CreatePipelineStateCache(cache_info, &pipeline_cache_);
  // D135: with no cache data (a first start, or a cache of another driver), the stored pipelines are compiled from
  // nothing, and the cache is saved once they are done; before, only pipelines new to the run counted, so a first
  // start that met none in play never wrote the cache.
  cache_was_empty_ = blob.empty();
  if (pipeline_cache_) pipelines_->SetCache(pipeline_cache_);

  // The shaders and pipelines of earlier runs: analyzed now, translated by the workers, and the pipelines
  // created as their shaders get ready (PrepareStored, every frame).
  std::filesystem::path records = storage;
  records += ".pipelines";
  storage_ = std::make_unique<PipelineStorage>();
  std::vector<PipelineStorage::Shader> shaders;
  std::vector<PipelineStorage::Translation> translations;
  std::vector<PipelineStorage::PipelineRecord> pipelines;
  if (!storage_->Open(records, shaders, translations, pipelines)) {
    storage_.reset();
    return;
  }
  for (const auto& shader : shaders) {
    shaders_->Get(xenos::ShaderType(shader.type), shader.ucode.data(), uint32_t(shader.ucode.size()));
  }
  bool pending = false;
  // Every stored translation, also those of draws that make no pipeline (no target, for one).
  for (const auto& translation : translations) {
    if (rg::SpirvShader* shader = shaders_->Find(xenos::ShaderType(translation.type), translation.hash)) {
      uint64_t modification = translation.modification;
      if (xenos::ShaderType(translation.type) == xenos::ShaderType::kPixel) {  // D164, as the pipelines below
        Translator::Modification m(modification);
        if (m.pixel.param_gen_enable) m.pixel.param_gen_texel_centre = param_gen_texel_centre_ ? 1 : 0;
        modification = m.value;
      }
      shaders_->GetHostShader(*shader, modification, pending);
    }
  }
  // Records from a device with D24_UNORM_S8_UINT depth, on one without it: its draws ask for the fallback (D103).
  const bool depth_unorm24 = DepthUnorm24Supported(render_device);
  for (const auto& record : pipelines) {
    // D137: an object motion pipeline: kept apart until a draw asks for object motion.
    if (record.pixel_hash == PipelineStorage::kMotionPixelHash) {
      Stored stored{shaders_->Find(xenos::ShaderType::kVertex, record.vertex_hash), record.vertex_modification,
                    nullptr, 0, record.state, true};
      if (!stored.vertex_shader) continue;
      if (!depth_unorm24 && stored.state.dsv_format == TEX_FORMAT_D24_UNORM_S8_UINT) {
        stored.state.dsv_format = TEX_FORMAT_D32_FLOAT_S8X24_UINT;
      }
      stored_motion_.push_back(stored);
      continue;
    }
    // D164: a stored pixel shader that reads its pixel position takes this run's way of giving it (a list stored
    // before D164, or with the other setting, matches the draws again).
    uint64_t pixel_modification = record.pixel_modification;
    {
      Translator::Modification m(pixel_modification);
      if (m.pixel.param_gen_enable) m.pixel.param_gen_texel_centre = param_gen_texel_centre_ ? 1 : 0;
      pixel_modification = m.value;
    }
    Stored stored{shaders_->Find(xenos::ShaderType::kVertex, record.vertex_hash), record.vertex_modification,
                  record.pixel_hash ? shaders_->Find(xenos::ShaderType::kPixel, record.pixel_hash) : nullptr,
                  pixel_modification, record.state};
    if (!stored.vertex_shader || (record.pixel_hash && !stored.pixel_shader)) continue;
    if (!depth_unorm24 && stored.state.dsv_format == TEX_FORMAT_D24_UNORM_S8_UINT) {
      stored.state.dsv_format = TEX_FORMAT_D32_FLOAT_S8X24_UINT;
    }
    // Queue the translations now; the workers start on them right away.
    shaders_->GetHostShader(*stored.vertex_shader, stored.vertex_modification, pending);
    if (stored.pixel_shader) shaders_->GetHostShader(*stored.pixel_shader, stored.pixel_modification, pending);
    stored_.push_back(stored);
  }
  shaders_->SetStorage(storage_.get());
  stored_total_ = uint32_t(stored_.size());
  stored_start_ = std::chrono::steady_clock::now();
  REXLOG_INFO("[native_x_renderer] pipeline storage {}: {} shaders, {} translations and {} pipelines from earlier runs; "
              "pipeline cache {} KB",
              records.string(), shaders.size(), translations.size(), pipelines.size(), blob.size() >> 10);
}

// D134: the stored pipelines' progress at start, for a game's setup screen (StorageProgress): read on any thread.
namespace {
std::atomic<uint32_t> g_stored_queued{0}, g_stored_done{0}, g_stored_loading{0}, g_stored_finished{0};
}  // namespace

void StoredPipelineProgress(uint32_t* queued, uint32_t* done, uint32_t* loading, uint32_t* finished) {
  if (queued) *queued = g_stored_queued.load(std::memory_order_relaxed);
  if (done) *done = g_stored_done.load(std::memory_order_relaxed);
  if (loading) *loading = g_stored_loading.load(std::memory_order_relaxed);
  if (finished) *finished = g_stored_finished.load(std::memory_order_acquire);
}

void SetStoredPipelinesFinished() {
  g_stored_finished.store(1, std::memory_order_release);
  SetCompileThreadsLow(true);  // D135: the game's threads first from now on
}

void DrawPath::PublishStoredProgress() {
  // A stored pipeline is done once DiligentCore finished compiling it (or it failed, or could not be created).
  for (size_t i = 0; i < stored_compiling_.size();) {
    Pipeline* p = stored_compiling_[i];
    if (p->pso && p->pso->GetStatus() == PIPELINE_STATE_STATUS_COMPILING) {
      ++i;
      continue;
    }
    ++stored_done_;
    stored_compiling_[i] = stored_compiling_.back();
    stored_compiling_.pop_back();
  }
  g_stored_queued.store(stored_total_, std::memory_order_relaxed);
  g_stored_done.store(stored_done_, std::memory_order_relaxed);
  g_stored_loading.store(1, std::memory_order_relaxed);
  if (stored_.empty() && stored_compiling_.empty() && !g_stored_finished.load(std::memory_order_relaxed)) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stored_start_).count();
    REXLOG_INFO("[native_x_renderer] pipeline storage: all {} stored pipelines compiled {:.0f} ms after the first draw",
                stored_total_, ms);
    SetStoredPipelinesFinished();
  }
}

void DrawPath::PrepareStored() {
  // D137: the stored object motion pipelines join once a draw asked for object motion (this frame's draws set it), and
  // are dropped without it (they would only cost compilation).
  if (!stored_motion_.empty()) {
    if (object_motion_on_ && EnsureMotionPixelShader()) {
      stored_total_ += uint32_t(stored_motion_.size());
      stored_.insert(stored_.end(), stored_motion_.begin(), stored_motion_.end());
    }
    stored_motion_.clear();
    stored_motion_.shrink_to_fit();
  }
  if (stored_.empty()) return;
  // A bounded number per frame: each creation is quick (DiligentCore compiles them on its threads).
  constexpr uint32_t kPerFrame = 64;
  uint32_t created = 0;
  for (size_t i = 0; i < stored_.size() && created < kPerFrame;) {
    Stored& s = stored_[i];
    bool vertex_pending = false, pixel_pending = false;
    if (s.motion) {
      // Its motion shader is made from the draw's own vertex shader once that is translated (GetMotionShader says
      // neither ready nor pending until then).
      IShader* base = shaders_->GetHostShader(*s.vertex_shader, s.vertex_modification, vertex_pending);
      IShader* vertex =
          base ? shaders_->GetMotionShader(*s.vertex_shader, s.vertex_modification, vertex_pending) : nullptr;
      if (vertex_pending) {
        ++i;
        continue;
      }
      if (vertex) {
        s.state.vertex_shader = vertex;
        s.state.pixel_shader = motion_pixel_shader_.RawPtr();
        bool made = false;
        if (Pipeline* p = pipelines_->Get(s.state, *s.vertex_shader, nullptr, &made, true)) {
          stored_compiling_.push_back(p);
          if (made && cache_was_empty_) ++pipelines_unsaved_;
        } else {
          ++stored_done_;
        }
        ++created;
      } else {
        ++stored_done_;
      }
      s = stored_.back();
      stored_.pop_back();
      continue;
    }
    IShader* vertex = shaders_->GetHostShader(*s.vertex_shader, s.vertex_modification, vertex_pending);
    IShader* pixel =
        s.pixel_shader ? shaders_->GetHostShader(*s.pixel_shader, s.pixel_modification, pixel_pending) : nullptr;
    if (vertex_pending || pixel_pending) {
      ++i;
      continue;
    }
    if (vertex && (pixel || !s.pixel_shader)) {
      s.state.vertex_shader = vertex;
      s.state.pixel_shader = pixel;
      bool made = false;
      if (Pipeline* p = pipelines_->Get(s.state, *s.vertex_shader, s.pixel_shader, &made)) {
        stored_compiling_.push_back(p);
        if (made && cache_was_empty_) ++pipelines_unsaved_;
      } else {
        ++stored_done_;
      }
      ++created;
    } else {
      ++stored_done_;  // a shader that could not be translated: nothing to compile
    }
    s = stored_.back();
    stored_.pop_back();
  }
  if (stored_.empty()) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stored_start_).count();
    REXLOG_INFO("[native_x_renderer] pipeline storage: the {} stored pipelines created in {:.0f} ms after the first draw "
                "(their compilation continues on DiligentCore's threads)",
                stored_total_, ms);
  }
}

void DrawPath::SavePipelineCache() {
  if (!pipeline_cache_ || pipeline_cache_path_.empty() || saving_) return;
  if (save_thread_.joinable()) save_thread_.join();  // the last save, finished
  // On a thread of its own (D45): the driver's serialization and the write. Vulkan's pipeline cache is
  // synchronized internally, so pipelines can be created meanwhile; if one is added between GetData's two
  // calls, GetData returns nothing and the save is tried again later. The file is written next to the
  // cache and renamed over it, so a process killed while writing leaves the last complete cache.
  saving_ = true;
  save_thread_ = std::thread([cache = pipeline_cache_, path = pipeline_cache_path_, created = pipelines_unsaved_,
                              this]() {
    auto start = std::chrono::steady_clock::now();
    RefCntAutoPtr<IDataBlob> blob;
    cache->GetData(&blob);
    bool saved = false;
    if (blob) {
      std::filesystem::path temporary = path;
      temporary += ".tmp";
      {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(blob->GetConstDataPtr()), std::streamsize(blob->GetSize()));
        saved = bool(file);
      }
      std::error_code error;
      if (saved) std::filesystem::rename(temporary, path, error);
      saved = saved && !error;
    }
    if (saved) {
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      REXLOG_INFO("[native_x_renderer] pipeline cache saved: {} KB, {} new pipelines, in {:.0f} ms on its own thread",
                  blob->GetSize() >> 10, created, ms);
    } else {
      save_failed_ = true;
    }
    saving_ = false;
  });
  pipelines_unsaved_ = 0;
}

rg::SpirvShader* DrawPath::FindShader(xenos::ShaderType type, const StateTracker::Program& program,
                                      bool* new_failure) {
  if (new_failure) *new_failure = false;
  ShaderEntry& entry = shader_entries_[(program.hash + uint32_t(type)) & (shader_entries_.size() - 1)];
  if (entry.shader && entry.hash == program.hash && entry.type == uint32_t(type)) return entry.shader;
  rg::SpirvShader* shader =
      shaders_->Get(type, program.ucode.data(), uint32_t(program.ucode.size()), program.hash, new_failure);
  if (shader) entry = {program.hash, uint32_t(type), shader};
  return shader;
}

IShader* DrawPath::FindHostShader(rg::SpirvShader& shader, uint64_t modification, bool& pending) {
  uint64_t key = reinterpret_cast<uintptr_t>(&shader) ^ (modification * 0x9E3779B97F4A7C15ull);
  HostShaderEntry& entry = host_shader_entries_[(key ^ (key >> 29)) & (host_shader_entries_.size() - 1)];
  if (entry.host && entry.shader == &shader && entry.modification == modification) {
    pending = false;
    return entry.host;
  }
  IShader* host = shaders_->GetHostShader(shader, modification, pending);
  if (host) entry = {&shader, modification, host};
  return host;
}

void DrawPath::AddDrawRanges(const StateTracker::Program& vertex_program, const rg::RegisterFile& regs,
                             std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (vertex_program.ucode.empty()) return;
  const uint32_t type = uint32_t(xenos::ShaderType::kVertex);
  const ShaderEntry& entry = shader_entries_[(vertex_program.hash + type) & (shader_entries_.size() - 1)];
  rg::SpirvShader* shader = entry.shader && entry.hash == vertex_program.hash && entry.type == type
                                ? entry.shader
                                : shaders_->Find(xenos::ShaderType::kVertex, vertex_program.hash);
  if (!shader) return;
  // RequestRanges refuses the whole request for one range out of guest memory.
  auto add = [&ranges](uint32_t start, uint32_t length) {
    if (length && start < rg::SharedMemory::kBufferSize && length <= rg::SharedMemory::kBufferSize - start) {
      ranges.emplace_back(start, length);
    }
  };
  // Vertices: as DrawInternal requests them.
  const rg::Shader::ConstantRegisterMap& constants = shader->constant_register_map();
  for (uint32_t i = 0; i < std::size(constants.vertex_fetch_bitmap); ++i) {
    uint32_t bits = constants.vertex_fetch_bitmap[i];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
      if (fetch.type == xenos::FetchConstantType::kVertex) add(fetch.address << 2, fetch.size << 2);
    }
  }
  // Indices from memory: the range the primitive processor requests for them.
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (initiator.source_select == xenos::SourceSelect::kDMA && initiator.num_indices) {
    uint32_t size_log2 = initiator.index_size == xenos::IndexFormat::kInt16 ? 1 : 2;
    uint32_t base = regs[rg::XE_GPU_REG_VGT_DMA_BASE] & ~((UINT32_C(1) << size_log2) - 1);
    add(base, uint32_t(initiator.num_indices) << size_log2);
  }
}

void DrawPath::UploadAhead(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t draws) {
  memory_->BeginWindow();
  if (ranges.empty()) return;
  memory_->RequestRanges(ranges.data(), ranges.size());
  ++stats_.ahead_batches;
  stats_.ahead_draws += draws;
}

void DrawPath::EndAheadWindow() {
  if (memory_) memory_->EndWindow();
}

void DrawPath::Draw(const StateTracker::Program& vertex_program, const StateTracker::Program& pixel_program,
                    std::span<const Copy> copies, const std::array<HostTarget*, 5>& targets, bool tiled) {
  ++stats_.draws;
  ScopedRegisters overrides(*registers_);
  rg::RegisterFile& regs = *registers_;
  // Host targets have one sample (D60): the translated shaders (alpha to coverage) and the interpolation
  // are those of a surface without MSAA.
  overrides.Set(rg::XE_GPU_REG_RB_SURFACE_INFO, regs[rg::XE_GPU_REG_RB_SURFACE_INFO] & ~(3u << 16));
  if (tiled) {
    // The tile's window offset and scissor are in bin-predicated packets, which the tracker skips. The
    // host target holds every tile (D60): no offset, no window scissor (the screen scissor still applies).
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_OFFSET, 0);
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, regs[rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] & 0x80000000u);
    overrides.Set(rg::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 0x2000u | (0x2000u << 16));
  }
  if (DrawInternal(vertex_program, pixel_program, copies, targets)) {
    ++stats_.drawn;
  }
}

bool DrawPath::DrawInternal(const StateTracker::Program& vertex_program, const StateTracker::Program& pixel_program,
                            std::span<const Copy> copies, const std::array<HostTarget*, 5>& targets) {
  const rg::RegisterFile& regs = *registers_;
  IDeviceContext* context = device_->context();
  const uint64_t copies_at_start = memory_->copies();  // D98

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) return false;

  // Shaders.
  // A program the SDK cannot analyze: where it came from, once (bring-up diagnostics).
  auto log_bad_program = [](const char* stage, const StateTracker::Program& program) {
    std::string dwords;
    for (size_t i = 0; i < program.ucode.size() && i < 512; ++i) {
      dwords += fmt::format(" {:08X}", rex::byte_swap(program.ucode[i]));
    }
    REXLOG_ERROR("[native_x_renderer] the {} program came from {} at {:#x} (packet {:#x}) in {}:{}", stage,
                 program.immediate ? "IM_LOAD_IMMEDIATE" : "IM_LOAD", program.address, program.packet,
                 CallName(program.loaded_by), dwords);
  };
  if (vertex_program.ucode.empty()) return false;
  bool new_failure = false;
  rg::SpirvShader* vertex_shader = FindShader(xenos::ShaderType::kVertex, vertex_program, &new_failure);
  if (!vertex_shader) {
    if (new_failure) log_bad_program("vertex", vertex_program);
    ++stats_.failed;
    return false;
  }
  if (vertex_shader->memexport_eM_written()) {
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] a vertex shader uses memory export: not supported yet");
    return false;
  }
  bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) return false;
  rg::SpirvShader* pixel_shader = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    if (!pixel_program.ucode.empty()) {
      pixel_shader = FindShader(xenos::ShaderType::kPixel, pixel_program, &new_failure);
      if (!pixel_shader) {
        if (new_failure) log_bad_program("pixel", pixel_program);
        ++stats_.failed;
        return false;
      }
      if (pixel_shader->memexport_eM_written()) {
        NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] a pixel shader uses memory export: not supported yet");
        return false;
      }
      if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) pixel_shader = nullptr;
    }
  }
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t param_gen_position = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? vertex_shader->writes_interpolators() &
                         pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                                regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_position)
                   : 0;

  // Primitives and indices (this may upload the index range).
  rg::PrimitiveProcessor::ProcessingResult primitives;
  if (!primitives_->Process(primitives)) {
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] primitive processing failed (primitive type {})",
            uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type));
    ++stats_.failed;
    return false;
  }
  static int32_t logged_draws = 0;
  if (logged_draws < REXCVAR_GET(native_x_renderer_draw_log)) {
    ++logged_draws;
    REXLOG_INFO("[native_x_renderer] draw {}: VGT_DRAW_INITIATOR {:08X} DMA base {:08X} size {:08X}; guest {} x{} -> host {} x{}, "
                "index buffer type {}, VS type {}, VS {:016X} PS {:016X}",
                logged_draws, regs[rg::XE_GPU_REG_VGT_DRAW_INITIATOR], regs[rg::XE_GPU_REG_VGT_DMA_BASE],
                regs[rg::XE_GPU_REG_VGT_DMA_SIZE], uint32_t(primitives.guest_primitive_type),
                primitives.guest_draw_vertex_count, uint32_t(primitives.host_primitive_type),
                primitives.host_draw_vertex_count, uint32_t(primitives.index_buffer_type),
                uint32_t(primitives.host_vertex_shader_type), vertex_shader->ucode_data_hash(),
                pixel_shader ? pixel_shader->ucode_data_hash() : 0);
  }
  {
    // Bring-up aids (environment, read once): NXR_DRAWS_AFTER_S=<s> logs 900 draws from that many seconds after the
    // first one, with their targets; NXR_SKIP_VS=<hash>[,<hash>...] skips the draws with those vertex shaders.
    static const double after_s = [] {
      const char* v = std::getenv("NXR_DRAWS_AFTER_S");
      return v && *v ? std::atof(v) : -1.0;
    }();
    static const std::vector<uint64_t> skip = [] {
      std::vector<uint64_t> out;
      if (const char* v = std::getenv("NXR_SKIP_VS")) {
        for (const char* q = v; *q;) {
          char* e = nullptr;
          uint64_t h = std::strtoull(q, &e, 16);
          if (e == q) break;
          out.push_back(h);
          q = *e == ',' ? e + 1 : e;
        }
      }
      return out;
    }();
    const uint64_t vs_hash = vertex_shader->ucode_data_hash();
    if (after_s >= 0) {
      static const auto first = std::chrono::steady_clock::now();
      static int dumped = 0;
      if (dumped < 900 && std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() > after_s) {
        ++dumped;
        REXLOG_INFO("[native_x_renderer] draw dump {}: prim {} count {} index type {}; VS {:016X} PS {:016X}; surface "
                    "{:08X} color {:08X} depth {:08X} modecontrol {:08X} depthcontrol {:08X}",
                    dumped, uint32_t(primitives.guest_primitive_type), primitives.guest_draw_vertex_count,
                    uint32_t(primitives.index_buffer_type), vs_hash, pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                    regs[0x2000], regs[0x2001], regs[0x2002], regs[0x2208], regs[0x2200]);
      }
    }
    // NXR_DUMP_VS=<hash>: that vertex shader's microcode (the SDK's disassembly) and the fetch constants of its first
    // draw, logged once.
    static const uint64_t dump_vs = [] {
      const char* v = std::getenv("NXR_DUMP_VS");
      return v && *v ? std::strtoull(v, nullptr, 16) : uint64_t(0);
    }();
    static bool dumped_vs = false;
    if (dump_vs && vs_hash == dump_vs && !dumped_vs) {
      dumped_vs = true;
      REXLOG_INFO("[native_x_renderer] VS {:016X} disassembly:\n{}", vs_hash, vertex_shader->ucode_disassembly());
      if (pixel_shader)
        REXLOG_INFO("[native_x_renderer] its PS {:016X} disassembly:\n{}", pixel_shader->ucode_data_hash(),
                    pixel_shader->ucode_disassembly());
      std::string fetch;
      for (uint32_t i = 0; i < 32 * 6; ++i) {
        if (i % 6 == 0) fetch += fmt::format("\n  fetch {:2}:", i / 6);
        fetch += fmt::format(" {:08X}", regs[0x4800 + i]);
      }
      REXLOG_INFO("[native_x_renderer] its fetch constants:{}", fetch);
      const auto& map = vertex_shader->constant_register_map();
      std::string used;
      for (uint32_t i = 0; i < std::size(map.vertex_fetch_bitmap); ++i) {
        used += fmt::format(" {:08X}", map.vertex_fetch_bitmap[i]);
        for (uint32_t bit = 0; bit < 32; ++bit) {
          if (!(map.vertex_fetch_bitmap[i] >> bit & 1)) continue;
          const auto f = regs.GetVertexFetch(i * 32 + bit);
          used += fmt::format(" [vf{} type {} address {:08X} size {} endian {}]", i * 32 + bit, uint32_t(f.type),
                              f.address << 2, f.size << 2, uint32_t(f.endian));
        }
      }
      REXLOG_INFO("[native_x_renderer] its vertex fetch bitmap:{}; textures used {:08X}", used,
                  vertex_shader->GetUsedTextureMaskAfterTranslation());
    }
    // NXR_VS_CONSTANTS=<hash> (a bring-up aid): the first draw a frame with that vertex shader: whether its vertex
    // float constants (c0 to c255) differ from the last frame's, counted and logged every 600 frames (an animation
    // that steps at the game's 30 Hz tick shows as changes on about a third of the frames at 110 FPS).
    {
      static const uint64_t watch_vs = [] {
        const char* v = std::getenv("NXR_VS_CONSTANTS");
        return v && *v ? std::strtoull(v, nullptr, 16) : uint64_t(0);
      }();
      static uint64_t frame_seen = ~0ull;
      static uint32_t frames = 0, changed = 0;
      static std::array<uint32_t, 256 * 4> last{};
      static std::array<uint32_t, 256> counts{};
      if (watch_vs && vs_hash == watch_vs && frame_seen != device_->frames_presented()) {
        frame_seen = device_->frames_presented();
        const uint32_t* c = &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X];
        bool any = false;
        for (uint32_t i = 0; i < 256; ++i) {
          if (std::memcmp(&c[i * 4], &last[i * 4], 16)) ++counts[i], any = true;
        }
        std::memcpy(last.data(), c, sizeof(last));
        // NXR_VS_CONSTANT_TRACE=<register>: that register and the next two (a bone's rows), every frame, for 1500 frames.
        {
          static const int trace_reg = [] {
            const char* v = std::getenv("NXR_VS_CONSTANT_TRACE");
            return v && *v ? std::atoi(v) : -1;
          }();
          static uint32_t traced = 0;
          if (trace_reg >= 0 && trace_reg < 254 && traced < 1500) {
            ++traced;
            float f[12];
            for (int k = 0; k < 12; ++k) {
              uint32_t u = c[trace_reg * 4 + k];
              std::memcpy(&f[k], &u, 4);
            }
            REXLOG_INFO("[native_x_renderer] VS constant trace: frame {} c{} {:.5f} {:.5f} {:.5f} {:.5f} | {:.5f} {:.5f} {:.5f} {:.5f} "
                        "| {:.5f} {:.5f} {:.5f} {:.5f}",
                        frame_seen, trace_reg, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
          }
        }
        ++frames;
        if (any) ++changed;
        if (frames == 600) {
          std::string per;
          for (uint32_t i = 0; i < 256; ++i)
            if (counts[i]) per += fmt::format(" c{}:{}", i, counts[i]);
          REXLOG_INFO("[native_x_renderer] VS {:016X} constants: changed on {} of {} frames; per register:{}", watch_vs,
                      changed, frames, per);
          frames = changed = 0;
          counts.fill(0);
        }
      }
    }
    // NXR_DUMP_PS=<hash>: that pixel shader's microcode, logged once.
    static const uint64_t dump_ps = [] {
      const char* v = std::getenv("NXR_DUMP_PS");
      return v && *v ? std::strtoull(v, nullptr, 16) : uint64_t(0);
    }();
    static bool dumped_ps = false;
    if (dump_ps && pixel_shader && pixel_shader->ucode_data_hash() == dump_ps && !dumped_ps) {
      dumped_ps = true;
      REXLOG_INFO("[native_x_renderer] PS {:016X} (VS {:016X}) disassembly:\n{}", dump_ps, vs_hash,
                  pixel_shader->ucode_disassembly());
    }
    if (std::find(skip.begin(), skip.end(), vs_hash) != skip.end()) return false;
    // NXR_SKIP_PS=<hash>[,<hash>...]: the same by pixel shader.
    static const std::vector<uint64_t> skip_ps = [] {
      std::vector<uint64_t> out;
      if (const char* v = std::getenv("NXR_SKIP_PS")) {
        for (const char* q = v; *q;) {
          char* e = nullptr;
          uint64_t h = std::strtoull(q, &e, 16);
          if (e == q) break;
          out.push_back(h);
          q = *e == ',' ? e + 1 : e;
        }
      }
      return out;
    }();
    if (pixel_shader && std::find(skip_ps.begin(), skip_ps.end(), pixel_shader->ucode_data_hash()) != skip_ps.end())
      return false;
  }
  if (!primitives.host_draw_vertex_count) return false;
  PRIMITIVE_TOPOLOGY topology;
  switch (primitives.host_primitive_type) {
    case xenos::PrimitiveType::kPointList: topology = PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: topology = PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default:
      NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] host primitive type {} not supported",
              uint32_t(primitives.host_primitive_type));
      ++stats_.failed;
      return false;
  }

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification, for a device without clip
  // and cull distances).
  Translator& translator = shaders_->translator();
  auto program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  xenos::PrimitiveType guest_primitive_type = regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type;
  Translator::Modification vertex_modification(translator.GetDefaultVertexShaderModification(
      vertex_shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg), primitives.host_vertex_shader_type));
  vertex_modification.vertex.interpolator_mask = interpolator_mask;
  vertex_modification.vertex.tessellation_mode = 0;
  vertex_modification.vertex.user_clip_plane_count = 0;
  vertex_modification.vertex.user_clip_plane_cull = 0;
  vertex_modification.vertex.point_ps_ucp_mode = clip_cntl.ps_ucp_mode;
  if (primitives.host_vertex_shader_type == rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    vertex_modification.vertex.output_point_parameters = uint32_t(param_gen_position != UINT32_MAX);
  } else {
    vertex_modification.vertex.output_point_parameters =
        uint32_t((vertex_shader->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 guest_primitive_type == xenos::PrimitiveType::kPointList);
  }
  vertex_modification.vertex.vertex_kill_and = 0;
  // Translated on a worker thread (D34): until then the draw is skipped.
  bool translating = false;
  IShader* host_vertex_shader = FindHostShader(*vertex_shader, vertex_modification.value, translating);
  if (!host_vertex_shader && !translating) {
    ++stats_.failed;
    return false;
  }
  IShader* host_pixel_shader = nullptr;
  uint64_t pixel_modification_value = 0;  // for the storage (D42)
  bool param_gen = false;                 // the pixel shader reads its pixel position
  if (pixel_shader) {
    Translator::Modification pixel_modification(translator.GetDefaultPixelShaderModification(
        pixel_shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    pixel_modification.pixel.interpolator_mask = interpolator_mask;
    pixel_modification.pixel.interpolators_centroid =
        interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                                                   regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                                                   regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (param_gen_position < xenos::kMaxInterpolators) {
      param_gen = true;
      pixel_modification.pixel.param_gen_enable = 1;
      pixel_modification.pixel.param_gen_interpolator = param_gen_position;
      pixel_modification.pixel.param_gen_point = uint32_t(guest_primitive_type == xenos::PrimitiveType::kPointList);
      pixel_modification.pixel.param_gen_texel_centre = param_gen_texel_centre_ ? 1 : 0;  // D164
    } else {
      pixel_modification.pixel.param_gen_enable = 0;
      pixel_modification.pixel.param_gen_interpolator = 0;
      pixel_modification.pixel.param_gen_point = 0;
    }
    using DepthStencilMode = Translator::Modification::DepthStencilMode;
    pixel_modification.pixel.depth_stencil_mode =
        pixel_shader->implicit_early_z_write_allowed() &&
                (!pixel_shader->writes_color_target(0) ||
                 !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    bool pixel_translating = false;
    pixel_modification_value = pixel_modification.value;
    host_pixel_shader = FindHostShader(*pixel_shader, pixel_modification.value, pixel_translating);
    if (!host_pixel_shader && !pixel_translating) {
      ++stats_.failed;
      return false;
    }
    translating |= pixel_translating;
  }
  // D101: a draw into an offscreen target waits for its translations (the workers finish them on their own); D128:
  // within the frame's budget when one is set.
  const auto wait_start = std::chrono::steady_clock::now();
  bool waited = false;
  auto may_wait = [&] {
    const auto elapsed = std::chrono::steady_clock::now() - wait_start;
    if (elapsed >= kPipelineWaitLimit) return false;
    return wait_budget_ms_ <= 0.0 ||
           frame_waited_ms_ + std::chrono::duration<double, std::milli>(elapsed).count() < wait_budget_ms_;
  };
  while (translating && wait_for_pipelines_ && may_wait()) {
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    translating = false;
    if (!host_vertex_shader) {
      host_vertex_shader = FindHostShader(*vertex_shader, vertex_modification.value, translating);
      if (!host_vertex_shader && !translating) {
        ++stats_.failed;
        return false;
      }
    }
    if (pixel_shader && !host_pixel_shader) {
      bool pixel_translating = false;
      host_pixel_shader = FindHostShader(*pixel_shader, pixel_modification_value, pixel_translating);
      if (!host_pixel_shader && !pixel_translating) {
        ++stats_.failed;
        return false;
      }
      translating |= pixel_translating;
    }
  }
  if (translating) {
    ++stats_.translating;
    return false;
  }

  // Render targets: the colors the pixel shader writes with a non-empty mask, depth if tested.
  uint32_t color_mask = pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets()) : 0;
  ITextureView* render_target_views[4] = {};
  uint32_t render_target_count = 0, samples = 0;
  PipelineState state;
  state.Reset();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) continue;
    HostTarget* target = targets[i];
    if (!target) {
      color_mask &= ~(0xFu << (4 * i));
      continue;
    }
    render_target_views[i] = target->view;
    state.rtv_formats[i] = uint16_t(target->format);
    render_target_count = i + 1;
    samples = target->samples;
  }
  HostTarget* depth = (depth_control.z_enable || depth_control.stencil_enable) ? targets[4] : nullptr;
  if (!render_target_count && !depth) {
    ++stats_.no_targets;
    return false;
  }
  if (depth) {
    if (samples && depth->samples != samples) {
      NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] color and depth targets with different sample counts");
      ++stats_.failed;
      return false;
    }
    samples = depth->samples;
    state.dsv_format = uint16_t(depth->format);
  }
  // The target the draw's size and scale are taken from: the first color target, else depth.
  const HostTarget* sized = depth;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (render_target_views[i]) {
      sized = targets[i];
      break;
    }
  }
  if (depth && (depth->reduced != sized->reduced || depth->shadow != sized->shadow)) {
    NATIVE_X_RENDERER_WARN_ONCE(
        "[native_x_renderer] color and depth targets at different scales (one reduced, D49, or a shadow map, D92)");
    ++stats_.failed;
    return false;
  }
  if (param_gen && sized->reduced) {
    // The translated shaders revert the render scale in the pixel position they read (D37), not the reduced
    // one: such a draw reads positions off by their ratio. None of POTCAWE's does (D49).
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] a pixel shader reads its pixel position in a view drawn at the reduced "
                           "scale: the position is off by the ratio of the scales");
  }

  // Pipeline state (VulkanPipelineCache::GetCurrentStateDescription, in DiligentCore terms).
  state.vertex_shader = host_vertex_shader;
  state.pixel_shader = host_pixel_shader;
  state.render_target_count = uint8_t(render_target_count);
  state.samples = uint8_t(samples);
  state.topology = uint8_t(topology);
  auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  state.cull_mode = uint8_t(CULL_MODE_NONE);
  if (polygonal) {
    if (mode_cntl.cull_front) state.cull_mode = uint8_t(CULL_MODE_FRONT);
    if (mode_cntl.cull_back) state.cull_mode = uint8_t(CULL_MODE_BACK);
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      if (!mode_cntl.cull_front) polygon_type = std::min(polygon_type, mode_cntl.polymode_front_ptype);
      if (!mode_cntl.cull_back) polygon_type = std::min(polygon_type, mode_cntl.polymode_back_ptype);
    }
    state.wireframe = polygon_type != xenos::PolygonType::kTriangles &&
                      device_->device()->GetDeviceInfo().Features.WireframeFill == DEVICE_FEATURE_STATE_ENABLED;
    // The final pixels are the SDK Vulkan path's (see the viewport below), so its winding rule holds.
    state.front_counter_clockwise = mode_cntl.face ? 0 : 1;
  }
  state.depth_clip = !(clip_cntl.clip_disable &&
                       device_->device()->GetDeviceInfo().Features.DepthClamp == DEVICE_FEATURE_STATE_ENABLED);
  uint8_t stencil_reference = 0;
  if (depth) {
    state.depth_enable = depth_control.z_enable;
    state.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    state.depth_func = depth_control.z_enable ? Compare(depth_control.zfunc) : Compare(xenos::CompareFunction::kAlways);
    if (depth_control.stencil_enable) {
      state.stencil_enable = 1;
      state.stencil_front[0] = Stencil(depth_control.stencilfail);
      state.stencil_front[1] = Stencil(depth_control.stencilzfail);
      state.stencil_front[2] = Stencil(depth_control.stencilzpass);
      state.stencil_front[3] = Compare(depth_control.stencilfunc);
      bool back = polygonal && depth_control.backface_enable;
      state.stencil_back[0] = back ? Stencil(depth_control.stencilfail_bf) : state.stencil_front[0];
      state.stencil_back[1] = back ? Stencil(depth_control.stencilzfail_bf) : state.stencil_front[1];
      state.stencil_back[2] = back ? Stencil(depth_control.stencilzpass_bf) : state.stencil_front[2];
      state.stencil_back[3] = back ? Compare(depth_control.stencilfunc_bf) : state.stencil_front[3];
      // One mask and reference for both faces: the back face's only when drawing back faces only (the
      // SDK's fallback for devices without separate masks).
      auto ref_mask = regs.Get<reg::RB_STENCILREFMASK>(back && mode_cntl.cull_front ? rg::XE_GPU_REG_RB_STENCILREFMASK_BF
                                                                                    : rg::XE_GPU_REG_RB_STENCILREFMASK);
      state.stencil_read_mask = uint8_t(ref_mask.stencilmask);
      state.stencil_write_mask = uint8_t(ref_mask.stencilwritemask);
      stencil_reference = uint8_t(ref_mask.stencilref);
    }
    float slope = 0.0f, constant = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope, constant);
    state.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(regs.Get<reg::RB_DEPTH_INFO>().depth_format, constant);
    // The slope is per host pixel, smaller at a higher scale: the factor grows with the target's scale, as on the
    // Xenos path (the larger axis, for more bias rather than less), so the bias stays what the game asked (D92).
    state.slope_scaled_depth_bias =
        slope * xenos::kPolygonOffsetScaleSubpixelUnit * std::max(sized->scale.x, sized->scale.y);
  } else {
    state.depth_func = Compare(xenos::CompareFunction::kAlways);
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    PipelineState::Target& target = state.targets[i];
    target.write_mask = uint8_t((color_mask >> (4 * i)) & 0xF);
    if (!target.write_mask) continue;
    auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    target.src = uint8_t(BlendFactor(blend.color_srcblend, false));
    target.dst = uint8_t(BlendFactor(blend.color_destblend, false));
    target.op = uint8_t(BlendOp(blend.color_comb_fcn));
    target.src_alpha = uint8_t(BlendFactor(blend.alpha_srcblend, true));
    target.dst_alpha = uint8_t(BlendFactor(blend.alpha_destblend, true));
    target.op_alpha = uint8_t(BlendOp(blend.alpha_comb_fcn));
    target.blend_enable = !(target.src == BLEND_FACTOR_ONE && target.dst == BLEND_FACTOR_ZERO &&
                            target.op == BLEND_OPERATION_ADD && target.src_alpha == BLEND_FACTOR_ONE &&
                            target.dst_alpha == BLEND_FACTOR_ZERO && target.op_alpha == BLEND_OPERATION_ADD);
    if (!target.blend_enable) target.src = target.dst = target.op = target.src_alpha = target.dst_alpha = target.op_alpha = 0;
  }
  // Alpha to coverage on host targets with one sample (D69). With the game's MSAA it covers a fraction of
  // each pixel's samples equal to the alpha, and the resolve blends them with what is behind; the shader's
  // dither does it in 2x2 pixels instead, which shows as dots. So the dither is off, a target that does not
  // blend blends by the alpha, and a draw that writes depth writes it where the alpha is at least 0.5
  // (FillSystemConstants): mostly covered pixels still hide what is drawn behind them later. A target that
  // blends already (GRAW fades objects with alpha blending and alpha to coverage together) keeps its blend:
  // somewhat more opaque than the game's MSAA where the alpha is between 0 and 1. On a surface the game draws at 1x,
  // the console dithers too, and its objects are solid there: the dither stays (D114; blending made GH Metallica's
  // hair see-through).
  bool guest_msaa = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X;
  bool alpha_to_coverage_blended = (guest_msaa ? alpha_to_coverage_blend_.msaa : alpha_to_coverage_blend_.one) &&
                                   pixel_shader && regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable;
  if (alpha_to_coverage_blended) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      PipelineState::Target& target = state.targets[i];
      if (!target.write_mask || target.blend_enable) continue;
      target.blend_enable = 1;
      target.src = target.src_alpha = uint8_t(BLEND_FACTOR_SRC_ALPHA);
      target.dst = target.dst_alpha = uint8_t(BLEND_FACTOR_INV_SRC_ALPHA);
      target.op = target.op_alpha = uint8_t(BLEND_OPERATION_ADD);
    }
  }
  // D163: whether each color target's last draw blended.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (render_target_views[i] && targets[i]) targets[i]->last_draw_blended = state.targets[i].blend_enable != 0;
  }
  bool created = false;
  Pipeline* pipeline = pipelines_->Get(state, *vertex_shader, pixel_shader, &created);
  if (created) {
    // Kept for the next run (D42).
    if (storage_) {
      storage_->AddPipeline({vertex_shader->ucode_data_hash(), vertex_modification.value,
                             pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                             pixel_modification_value, state});
    }
    ++pipelines_unsaved_;
    frames_since_created_ = 0;
  }
  if (!pipeline) {
    NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] pipeline creation failed (more failures are not logged)");
    ++stats_.failed;
    return false;
  }
  // D101: a draw into an offscreen target waits for its pipeline (DiligentCore's threads compile it).
  while (wait_for_pipelines_ && !pipeline->failed && !pipelines_->Ready(*pipeline) && may_wait()) {
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  if (waited) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
    frame_waited_ms_ += ms;
    ++stats_.waited;
    stats_.waited_ms += ms;
    stats_.max_waited_ms = std::max(stats_.max_waited_ms, ms);
  }
  if (!pipelines_->Ready(*pipeline)) {
    // Still compiling (D28), or failed (logged): the draw is skipped.
    ++(pipeline->failed ? stats_.failed : stats_.compiling);
    return false;
  }

  // Viewport and scissor. DiligentCore's Vulkan backend flips Y with a negative viewport height, as
  // Direct3D does, so the translated shaders produce bottom-left-origin NDC (the SDK's D3D12 setting).
  // Both are computed in guest pixels and scaled when they are set (D37): the viewport's NDC transform is
  // relative to it, and Vulkan takes a fractional viewport.
  // Float24 depth keeps the guest's values (full_float24_in_0_to_1 false): the SDK halves it to move EDRAM
  // data through depth and doubles it back in its resolves, but here resolves copy host depth as it is,
  // and shaders compare sampled depth with depth they compute (shadow maps).
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, true, 16384, 16384, true, depth_control, false, false,
                                 pixel_shader && pixel_shader->writes_depth(), viewport_info);
  if (!viewport_info.xy_extent[0] || !viewport_info.xy_extent[1]) return false;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  if (!scissor.extent[0] || !scissor.extent[1]) return false;

  // Textures: loaded (or found up to date) before the vertex data, as the SDK does; the loads upload the
  // guest memory they read.
  uint32_t used_texture_mask = vertex_shader->GetUsedTextureMaskAfterTranslation() |
                               (pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  guest_textures_->RequestTextures(used_texture_mask);

  // Vertex data: every vertex fetch constant the vertex shader uses.
  const rg::Shader::ConstantRegisterMap& vertex_constants = vertex_shader->constant_register_map();
  {
    // A draw that fetches vertices through a constant that is not a vertex fetch (type 2, "invalid") is dropped, as
    // the SDK's command processors drop it (unless gpu_allow_invalid_fetch_constants): its data would not be in the
    // guest memory mirror, and the draw would take whatever vertices are there (Battlefield 1943's terrain patches: a
    // huge black mountain over the scene, which the Xenos path never drew).
    static const bool allow_invalid = rex::cvar::GetFlagByName("gpu_allow_invalid_fetch_constants") == "true";
    for (uint32_t i = 0; !allow_invalid && i < std::size(vertex_constants.vertex_fetch_bitmap); ++i) {
      uint32_t bits = vertex_constants.vertex_fetch_bitmap[i];
      uint32_t bit;
      while (rex::bit_scan_forward(bits, &bit)) {
        bits &= ~(1u << bit);
        if (regs.GetVertexFetch(i * 32 + bit).type != xenos::FetchConstantType::kVertex) {
          NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] a draw fetches vertices through a fetch constant that is "
                                      "not a vertex fetch (constant {}): dropped, as the SDK drops it",
                                      i * 32 + bit);
          ++stats_.failed;
          return false;
        }
      }
    }
  }
  uint32_t first_vertex_address = 0;  // for the object motion's key (D48)
  for (uint32_t i = 0; i < std::size(vertex_constants.vertex_fetch_bitmap); ++i) {
    uint32_t bits = vertex_constants.vertex_fetch_bitmap[i];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
      if (fetch.type != xenos::FetchConstantType::kVertex) continue;
      if (!first_vertex_address) first_vertex_address = fetch.address;
      memory_->RequestRange(fetch.address << 2, fetch.size << 2);
      // D140: vertices fetched from guest memory a resolve wrote this frame (a game that computes vertex data on the GPU
      // and resolves it to memory: Battlefield 1943's particle lighting). Resolves stay on the host, so the mirror held
      // what guest memory held there (zeros: black smoke; later whatever the memory held before: very bright, flickering
      // smoke and fire). The texture is marked: from its next resolve on it is also written into the mirror.
      if (resolved_) {
        {  // NXR_D140_LOG (a diagnosis aid): each draw reading a resolve's memory as vertices, and how old its resolve is
          static const bool log = std::getenv("NXR_D140_LOG") != nullptr;
          if (log) {
            if (ResolvedTexture* t = resolved_->VertexSource(fetch.address << 2, fetch.size << 2))
              REXLOG_INFO("[native_x_renderer] D140: draw reads {:#x} (range {:#x} +{:#x}), vertex data {}",
                          t->address, fetch.address << 2, fetch.size << 2, t->vertex_data);
          }
        }
        if (ResolvedTexture* t = resolved_->VertexSource(fetch.address << 2, fetch.size << 2); t && !t->vertex_data) {
          t->vertex_data = true;
          // D155: its newest resolve into the mirror now, for this draw (from its next resolve on, the resolve writes
          // it): before, this frame read what the mirror held, one frame of old data each time a buffer was found.
          if (blit_ && t->last_layout.valid && t->last_layout.address == t->address && t->ArrayView()) {
            const ResolvedTexture::GuestLayout& l = t->last_layout;
            Blitter::MirrorWrite write;
            write.address = l.address, write.width = l.width, write.height = l.height, write.pitch = l.pitch;
            write.guest_format = l.guest_format, write.tiled = l.tiled, write.endian = l.endian;
            write.scale_x = l.scale_x, write.scale_y = l.scale_y, write.slice = l.slice;
            if (blit_->WriteToMirror(context, t->ArrayView(), memory_->buffer(), memory_->uav(), write)) {
              TargetsChanged();  // the compute pass bound its own
              REXLOG_INFO("[native_x_renderer] D155: the resolve at {:#x} written to the mirror before its first draw",
                          t->address);
            }
          }
          static uint32_t logged = 0;
          if (logged++ < 16)
            REXLOG_INFO("[native_x_renderer] D140: a draw reads the guest memory of a resolve's texture at {:#x} ({}x{}, "
                        "guest format {}, {} bytes) as vertices (VS {:016X}): its resolves are written to the guest "
                        "memory mirror from now on",
                        t->address, t->guest_width, t->guest_height, t->guest_format, t->guest_bytes,
                        vertex_shader->ucode_data_hash());
        }
      }
    }
  }
  // An inline draw's data, if the game reused its memory since the draw was captured (D71).
  for (const Copy& copy : copies) {
    if (memory_->WriteIfChanged(context, copy.address, copy.data, copy.size)) ++stats_.inline_restored;
  }
  if (primitives_->UploadConverted()) ++stats_.copies;
  // This draw's own data or textures copied, ending the render pass: the queued draws' data copied now too (D98).
  if (upload_ahead_ && memory_->copies() != copies_at_start) upload_ahead_();
  stats_.copies += memory_->TakeCopies();
  stats_.uploaded_bytes += memory_->TakeUploadedBytes();
  stats_.unchanged_bytes += memory_->TakeUnchangedBytes();
  stats_.unwatched_hashed_bytes += memory_->TakeUnwatchedHashedBytes();
  memory_->PrepareForRead(context);

  // Constants.
  DrawContext draw{&regs, &primitives, &viewport_info, polygonal, used_texture_mask, alpha_to_coverage_blended,
                   state.depth_enable && state.depth_write};
  FillSystemConstants(draw);
  {
    MappedBuffer map(context, system_constants_buffer_);
    std::memcpy(map.data(), &system_constants_, sizeof(system_constants_));
  }
  const rg::SpirvShader* stage_shaders[2] = {vertex_shader, pixel_shader};
  // The vertex shader's are also kept for the object motion (D48).
  vertex_constants_.clear();
  vertex_registers_.clear();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader || !shader->constant_register_map().float_count) continue;
    MappedBuffer map(context, pipeline->float_constants[stage]);
    uint8_t* out = map.data();
    const rg::Shader::ConstantRegisterMap& constants = shader->constant_register_map();
    uint32_t first = stage ? rg::XE_GPU_REG_SHADER_CONSTANT_256_X : rg::XE_GPU_REG_SHADER_CONSTANT_000_X;
    bool keep = !stage && (object_motion_on_ || camera_vote_on_ || tick_interpolation_ || model_lamp_counts_ ||
                           (hud_classify_ && aspect_box_));
    uint8_t* const mapped_start = out;
    // A constant that holds the size of a resolved texture the shader samples, as the game wrote it for the guest
    // texture: its host size instead (D93).
    uint32_t sized_constant = UINT32_MAX;
    float host_size[4];
    for (const Profile::TextureSizeConstant& entry : texture_size_constants_) {
      if (entry.pixel_shader != (stage == 1) || entry.constant >= 256 || entry.fetch_constant >= 32 ||
          !(constants.float_bitmap[entry.constant >> 6] & (uint64_t(1) << (entry.constant & 63))) ||
          !(shader->GetUsedTextureMaskAfterTranslation() & (1u << entry.fetch_constant))) {
        continue;
      }
      const ResolvedTexture* texture = textures_->Resolved(regs.GetTextureFetch(entry.fetch_constant));
      if (!texture || (texture->width == texture->guest_width && texture->height == texture->guest_height)) continue;
      float game[4];
      std::memcpy(game, &regs[first + (entry.constant << 2)], sizeof(game));
      auto equal = [](float a, float b) { return std::abs(a - b) <= std::abs(b) * 1e-5f; };
      float guest_width = float(texture->guest_width), guest_height = float(texture->guest_height);
      if (!equal(game[0], guest_width) || !equal(game[1], guest_height) || !equal(game[2], 1.0f / guest_width) ||
          !equal(game[3], 1.0f / guest_height)) {
        continue;
      }
      sized_constant = entry.constant;
      host_size[0] = float(texture->width);
      host_size[1] = float(texture->height);
      host_size[2] = 1.0f / host_size[0];
      host_size[3] = 1.0f / host_size[1];
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("[native_x_renderer] the {} shader's c{} holds the size of the texture at fetch constant {}: its "
                    "host size {}x{} instead of the game's {}x{} (D93)",
                    stage ? "pixel" : "vertex", entry.constant, entry.fetch_constant, texture->width, texture->height,
                    texture->guest_width, texture->guest_height);
      }
      break;
    }
    // A run of consecutive constants at a time (the register file holds them in order): a shader's constants are
    // mostly whole ranges (matrices, bones, lights), and one copy per constant was a tenth of the renderer thread's
    // time in Kameo (D100).
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = constants.float_bitmap[i];
      while (bits) {
        uint32_t start = uint32_t(std::countr_zero(bits));
        uint32_t count = uint32_t(std::countr_zero(~(bits >> start)));
        bits &= count >= 64 ? 0 : ~(((uint64_t(1) << count) - 1) << start);
        uint32_t first_register = (i << 6) + start;
        size_t bytes = size_t(count) * 16;
        std::memcpy(out, &regs[first + (first_register << 2)], bytes);
        if (sized_constant >= first_register && sized_constant < first_register + count) {
          std::memcpy(out + size_t(sized_constant - first_register) * 16, host_size, 16);
        }
        if (keep) {
          // From the register file, not from `out`: the mapped buffer is write-combined memory, slow to read back
          // (with DLAA in a GH Metallica song, a sixth of the renderer thread's time, D120).
          size_t kept = vertex_constants_.size();
          const auto* source = reinterpret_cast<const uint8_t*>(&regs[first + (first_register << 2)]);
          vertex_constants_.insert(vertex_constants_.end(), source, source + bytes);
          if (sized_constant >= first_register && sized_constant < first_register + count) {
            std::memcpy(vertex_constants_.data() + kept + size_t(sized_constant - first_register) * 16, host_size, 16);
          }
          for (uint32_t k = 0; k < count; ++k) vertex_registers_.push_back(uint8_t(first_register + k));
        }
        out += bytes;
      }
    }
    // D154: a model placed from another camera than the frame's, moved onto it (SetAttachedCameraMove).
    if (!stage) MoveAttached(reinterpret_cast<const uint32_t*>(&regs[0]), first, constants.float_bitmap, mapped_start);
    // NXR_NEAR_DUMP=<seconds>,<frames> (a diagnosis aid): from that many seconds after the first draw, on that many
    // frames, every draw whose vertex shader's first perspective block is a near projection (near plane under half a
    // metre: first person models and the nearest scenery): its shader, the block's register and every float constant
    // it uses, as floats.
    if (!stage) {
      static const std::pair<double, int> near_dump = [] {
        const char* v = std::getenv("NXR_NEAR_DUMP");
        if (!v || !*v) return std::pair<double, int>(-1.0, 0);
        const char* comma = std::strchr(v, ',');
        return std::pair<double, int>(std::atof(v), comma ? std::atoi(comma + 1) : 4);
      }();
      if (near_dump.first >= 0) {
        static const auto since = std::chrono::steady_clock::now();
        static uint64_t first_frame = 0;
        const uint64_t now_frame = device_->frames_presented();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count() >= near_dump.first) {
          if (!first_frame) first_frame = now_frame;
          if (now_frame < first_frame + uint64_t(near_dump.second)) {
            std::vector<uint8_t> numbers, values;
            for (uint32_t i = 0; i < 4; ++i) {
              uint64_t bits = constants.float_bitmap[i];
              while (bits) {
                uint32_t bit = uint32_t(std::countr_zero(bits));
                bits &= bits - 1;
                uint32_t reg = (i << 6) + bit;
                numbers.push_back(uint8_t(reg));
                const auto* src = reinterpret_cast<const uint8_t*>(&regs[first + (reg << 2)]);
                values.insert(values.end(), src, src + 16);
              }
            }
            uint32_t index = 0;
            bool transposed = false;
            GtaoProjection projection;
            if (CameraVote::FindMatrix(values.data(), numbers.data(), uint32_t(numbers.size()), index, transposed) &&
                GtaoProjection::FromMatrix(CameraVote::MatrixAt(values.data(), index, transposed), projection) &&
                projection.near_plane() > 0.0f && projection.near_plane() < 0.5f) {
              std::string line = fmt::format("[native_x_renderer] near draw: frame {} vs {:016X} block c{}{} near {:.3f}:",
                                             now_frame, shader->ucode_data_hash(), numbers[index],
                                             transposed ? " (columns)" : "", projection.near_plane());
              for (size_t k = 0; k < numbers.size() && k < 40; ++k) {
                float f[4];
                std::memcpy(f, values.data() + k * 16, 16);
                line += fmt::format(" c{}=({:.4f} {:.4f} {:.4f} {:.4f})", numbers[k], f[0], f[1], f[2], f[3]);
              }
              REXLOG_INFO("{}", line);
            }
          }
        }
      }
    }
    // D138: the object's world-view-projection between its last two ticks, in the GPU's copy only.
    if (!stage && tick_interpolation_ && state.depth_write && !vertex_registers_.empty() && sized &&
        sized->guest_width == jitter_size_[0] && sized->guest_height == jitter_size_[1]) {
      uint32_t first_vertex = 0;
      for (uint32_t i = 0; i < std::size(vertex_constants.vertex_fetch_bitmap) && !first_vertex; ++i) {
        uint32_t bits = vertex_constants.vertex_fetch_bitmap[i];
        uint32_t bit;
        while (!first_vertex && rex::bit_scan_forward(bits, &bit)) {
          bits &= ~(1u << bit);
          xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
          if (fetch.type == xenos::FetchConstantType::kVertex) first_vertex = fetch.address;
        }
      }
      struct {
        uint64_t vertex_shader;
        uint32_t vertex_address, index_address, vertex_count, index_type;
      } key{vertex_shader->ucode_data_hash(), first_vertex, regs[rg::XE_GPU_REG_VGT_DMA_BASE],
            primitives.guest_draw_vertex_count, uint32_t(primitives.index_buffer_type)};
      // The first perspective-looking block only: blending the others too (shadow and light projections) darkened
      // the whole frame (run vcam8).
      tick_trace_shader_ = vertex_shader->ucode_data_hash();
      TickInterpolate(XXH3_64bits(&key, sizeof(key)), vertex_constants_.data(), vertex_registers_.data(),
                      uint32_t(vertex_registers_.size()), mapped_start, 0);
    }
  }
  // The bool/loop constants: uploaded when they differ from the last upload of this frame (D81); they rarely
  // change. The fetch constants change with nearly every draw (its textures): compared, they cost more than the
  // upload they would save.
  uint64_t frame = device_->frames_presented();
  const auto* bool_loop = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]);
  if (uploaded_frame_ != frame || std::memcmp(uploaded_bool_loop_.data(), bool_loop, kBoolLoopConstantsSize)) {
    MappedBuffer map(context, bool_loop_constants_buffer_);
    std::memcpy(map.data(), bool_loop, kBoolLoopConstantsSize);
    std::memcpy(uploaded_bool_loop_.data(), bool_loop, kBoolLoopConstantsSize);
    uploaded_frame_ = frame;
  }
  {
    MappedBuffer map(context, fetch_constants_buffer_);
    std::memcpy(map.data(), &regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
  }

  // Textures and samplers. Drawn at the reduced scale (D49), the samplers take the mip bias for it: log2 of
  // the reduced scale over the render scale (NVIDIA's DLSS guide), for the detail the upscaler brings back.
  float lod_bias = sized->reduced ? std::log2(sized->scale.x / scale_.x) : 0.0f;
  // Each variable set only when its object changes (D82): in Kameo's first level 2 % of them do. With binding sets
  // (D111) the objects are listed instead, per stage the textures then the samplers, and the set for them found.
  bool bindings_changed = false;
  const bool binding_sets = pipeline->binding_sets_on;
  if (binding_sets) binding_objects_.clear();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader) continue;
    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    for (size_t i = 0; i < texture_bindings.size() && i < pipeline->textures[stage].size(); ++i) {
      if (binding_sets) {
        IDeviceObject* view = nullptr;
        if (pipeline->textures[stage][i]) {
          const auto& binding = texture_bindings[i];
          view = textures_->View(binding.fetch_constant, regs.GetTextureFetch(binding.fetch_constant),
                                 binding.dimension, binding.is_signed);
        }
        binding_objects_.push_back(view);
        continue;
      }
      if (IShaderResourceVariable* variable = pipeline->textures[stage][i]) {
        const auto& binding = texture_bindings[i];
        IDeviceObject* view = textures_->View(binding.fetch_constant, regs.GetTextureFetch(binding.fetch_constant),
                                              binding.dimension, binding.is_signed);
        if (pipeline->texture_objects[stage][i] != view) {
          variable->Set(view);
          pipeline->texture_objects[stage][i] = view;
          ++stats_.bindings_set;
          bindings_changed = true;
        }
        pipeline->textures_set = true;
      }
    }
    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    for (size_t i = 0; i < sampler_bindings.size() && i < pipeline->samplers[stage].size(); ++i) {
      if (binding_sets) {
        binding_objects_.push_back(
            pipeline->samplers[stage][i]
                ? textures_->Sampler(regs.GetTextureFetch(sampler_bindings[i].fetch_constant), sampler_bindings[i],
                                     lod_bias)
                : nullptr);
        continue;
      }
      if (IShaderResourceVariable* variable = pipeline->samplers[stage][i]) {
        IDeviceObject* sampler =
            textures_->Sampler(regs.GetTextureFetch(sampler_bindings[i].fetch_constant), sampler_bindings[i], lod_bias);
        if (pipeline->sampler_objects[stage][i] != sampler) {
          variable->Set(sampler);
          pipeline->sampler_objects[stage][i] = sampler;
          ++stats_.bindings_set;
          bindings_changed = true;
        }
      }
    }
  }

  // D111: the pipeline's binding set for these objects, created on first use.
  BindingSet* set = nullptr;
  if (binding_sets) {
    bool created = false;
    set = pipelines_->FindSet(*pipeline, binding_objects_, device_->frames_presented(), &created);
    if (!set) {
      ++stats_.failed;
      return false;
    }
    if (created) ++stats_.binding_sets_created;
  }

  // Record the draw. The targets are bound only when they change (D30).
  std::array<ITextureView*, 5> views{render_target_views[0], render_target_views[1], render_target_views[2],
                                     render_target_views[3], depth ? depth->view : nullptr};
  if (!bound_valid_ || bound_count_ != render_target_count || bound_ != views) {
    context->SetRenderTargets(render_target_count, render_target_views, views[4],
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    bound_ = views;
    bound_count_ = render_target_count;
    bound_valid_ = true;
    viewport_valid_ = false;  // DiligentCore sets the viewport to the targets' size (D81)
    committed_ = nullptr;     // and the shader resources are committed again, transitioning what was a target (D82)
  }
  context->SetPipelineState(pipeline->pso);
  // The shader resources are committed when something changed since the last draw committed them (D82): another
  // pipeline, a texture or sampler, the render targets, or a texture no longer readable (loaded, copied or drawn
  // into since). DiligentCore allocates and writes a descriptor set for the textures and samplers on every
  // commit; in Kameo's first level three draws in four have the last draw's pipeline and resources.
  if (set) {
    // D111: the set's binding, its variables written when it was created; committed when it is not the last
    // committed one, or one of its textures is no longer readable.
    if (committed_ != set || !TexturesReadable(*set)) {
      context->CommitShaderResources(set->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      committed_ = set;
      ++stats_.committed;
    }
  } else if (bindings_changed || committed_ != pipeline || !TexturesReadable(*pipeline)) {
    context->CommitShaderResources(pipeline->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    committed_ = pipeline;
    ++stats_.committed;
  }
  pipeline->last_drawn = device_->frames_presented();
  // Guest pixels to host pixels: the target's scale per axis (the render scale, D37, D38; the reduced one,
  // D49); in a 2D layer kept at the game's aspect ratio (D43), the smaller of the two on both axes, and an
  // offset that centers the target's guest area in its host size.
  const RenderScale& target_scale = sized->scale;
  float scale_x = target_scale.x, scale_y = target_scale.y, offset_x = 0.0f, offset_y = 0.0f;
  HudPlace place;
  place.align = aspect_align_;
  if (aspect_box_ && hud_classify_ && target_scale.x != target_scale.y)
    place = ClassifyHudDraw(copies, vertex_program.hash);
  if (aspect_box_ && place.mode != HudMode::kStretch && target_scale.x != target_scale.y) {
    float uniform = std::min(target_scale.x, target_scale.y);
    const float gw = float(sized->guest_width), gh = float(sized->guest_height);
    offset_x = gw * (target_scale.x - uniform) * place.align;
    offset_y = gh * (target_scale.y - uniform) * 0.5f;
    scale_x = scale_y = uniform;
    if (place.mode == HudMode::kPoint) {
      // Its centre where the full picture puts it (the game placed it from the widened camera), its size the box's:
      // across on a wider output, and down on a taller one (16:10, 4:3), where the box sits in the middle of the
      // height and a marker moved across only sat up to 5 percent of the height off its flag (the review of the
      // pull request, 13 Nov 2026).
      // Every piece of one marker moves by the marker's own place when it is known (13 Nov: a plane's gunsight, drawn
      // in several pieces around one projected point, came apart and shifted in flight).
      const double g_cx = place.point_x >= 0 ? place.point_x / 1280.0 * gw : ((place.lo + place.hi) * 0.5 + 1.0) * 0.5 * gw;
      const double g_cy = (1.0 - (place.y_lo + place.y_hi) * 0.5) * 0.5 * gh;
      offset_x = float(g_cx * (target_scale.x - uniform));
      offset_y = float(g_cy * (target_scale.y - uniform));
    }
    if (place.mode == HudMode::kExtendLeft || place.mode == HudMode::kExtendRight) {
      // From the screen's edge to where its inner edge is in the middle box: the guest's x to host x linearly.
      const float box_offset = gw * (target_scale.x - uniform) * 0.5f;
      // The canvas's edge (not a part off it) is pinned to the screen's: a menu's backing that starts off the canvas
      // (12 Nov 21:00: "Set to flag spawn") was cut by the scissor at a canvas edge mapped into the screen.
      const float g_lo = std::max(0.0f, float((place.lo + 1.0) * 0.5) * gw);
      const float g_hi = std::min(gw, float((place.hi + 1.0) * 0.5) * gw);
      if (g_hi - g_lo > 0.5f) {
        if (place.mode == HudMode::kExtendLeft) {
          scale_x = (box_offset + g_hi * uniform) / (g_hi - g_lo);
          offset_x = -g_lo * scale_x;
        } else {
          const float inner = box_offset + g_lo * uniform;
          scale_x = (gw * target_scale.x - inner) / (g_hi - g_lo);
          offset_x = inner - g_lo * scale_x;
        }
      }
    }
  }
  // As RenderScale::HostX/HostY outside a box.
  auto host_x = [&](int64_t x) { return int32_t(std::floor(double(offset_x) + double(x) * scale_x + 0.5)); };
  auto host_y = [&](int64_t y) { return int32_t(std::floor(double(offset_y) + double(y) * scale_y + 0.5)); };
  Viewport viewport;
  viewport.TopLeftX = offset_x + float(viewport_info.xy_offset[0]) * scale_x;
  viewport.TopLeftY = offset_y + float(viewport_info.xy_offset[1]) * scale_y;
  viewport.Width = float(viewport_info.xy_extent[0]) * scale_x;
  viewport.Height = float(viewport_info.xy_extent[1]) * scale_y;
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  // The temporal antialiasing's jitter (D47): the main 3D view's image is shifted by a sub-pixel offset,
  // different every frame. The scissor stays. A view found from the draws (D119) starts at its first depth-writing
  // draw of that size with a perspective matrix.
  bool output_sized = sized->guest_width == jitter_size_[0] && sized->guest_height == jitter_size_[1];
  if (projection_vote_ && output_sized && state.depth_write && !vertex_registers_.empty() && projections_.size() < 64) {
    uint32_t matrix = 0;
    bool transposed = false;
    GtaoProjection p;
    if (CameraVote::FindMatrix(vertex_constants_.data(), vertex_registers_.data(), uint32_t(vertex_registers_.size()),
                               matrix, transposed) &&
        GtaoProjection::FromMatrix(CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed), p)) {
      auto close = [](float a, float b) { return std::abs(a - b) <= 0.01f * std::max(std::abs(a), std::abs(b)); };
      bool found = false;
      for (auto& [q, n] : projections_) {
        if (close(q.px, p.px) && close(q.py, p.py) && close(q.near_plane(), p.near_plane()) &&
            close(q.far_plane(), p.far_plane())) {
          ++n, found = true;
          break;
        }
      }
      if (!found) projections_.push_back({p, 1}), projection_ranges_.push_back({viewport_info.z_min, viewport_info.z_max});
    }
  }
  if (waiting_for_scene_ && !jitter_on_ && output_sized && state.depth_write && !vertex_registers_.empty()) {
    uint32_t matrix = 0;
    bool transposed = false;
    if (CameraVote::FindMatrix(vertex_constants_.data(), vertex_registers_.data(), uint32_t(vertex_registers_.size()),
                               matrix, transposed)) {
      jitter_on_ = jitter_scene_;
      waiting_for_scene_ = false;
      scene_started_ = true;
      scene_start_matrix_ = CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed);
    }
  }
  bool jittered = jitter_on_ && output_sized;
  if (jittered) {
    viewport.TopLeftX += jitter_[0];
    viewport.TopLeftY += jitter_[1];
    jittered_z_[0] = viewport_info.z_min;
    jittered_z_[1] = viewport_info.z_max;
  }
  Rect scissor_rect{host_x(scissor.offset[0]), host_y(scissor.offset[1]),
                    host_x(int64_t(scissor.offset[0]) + scissor.extent[0]),
                    host_y(int64_t(scissor.offset[1]) + scissor.extent[1])};
  // Never left of or above the target: a HUD draw reaching to a screen edge (HudMode::kExtendLeft, kExtendRight) maps
  // the canvas's x 0 to a negative host x (3440x1440, a quad from guest x 1000: about -1131), and Vulkan requires a
  // scissor's offset to be 0 or more (found in the review of the ultrawide pull request, 13 Nov 2026).
  auto on_target = [](Rect& r) {
    r.left = std::max(r.left, 0), r.top = std::max(r.top, 0);
    r.right = std::max(r.right, r.left), r.bottom = std::max(r.bottom, r.top);
  };
  on_target(scissor_rect);
  // HudMode::kOversize: its first pass stretched with the 3D view (no box).
  const bool oversize = place.mode == HudMode::kOversize;
  Viewport stretched_viewport = viewport;
  Rect stretched_scissor = scissor_rect;
  if (oversize) {
    const float sx = target_scale.x, sy = target_scale.y;
    stretched_viewport.TopLeftX = float(viewport_info.xy_offset[0]) * sx;
    stretched_viewport.TopLeftY = float(viewport_info.xy_offset[1]) * sy;
    stretched_viewport.Width = float(viewport_info.xy_extent[0]) * sx;
    stretched_viewport.Height = float(viewport_info.xy_extent[1]) * sy;
    auto sxp = [&](int64_t x) { return int32_t(std::floor(double(x) * sx + 0.5)); };
    auto syp = [&](int64_t y) { return int32_t(std::floor(double(y) * sy + 0.5)); };
    stretched_scissor = Rect{sxp(scissor.offset[0]), syp(scissor.offset[1]),
                             sxp(int64_t(scissor.offset[0]) + scissor.extent[0]),
                             syp(int64_t(scissor.offset[1]) + scissor.extent[1])};
    on_target(stretched_scissor);
  }
  // Set when they change (D81): DiligentCore records them on every call.
  if (!viewport_valid_ || std::memcmp(&viewport, &viewport_, sizeof(viewport))) {
    context->SetViewports(1, &viewport, 0, 0);
    viewport_ = viewport;
  }
  if (!viewport_valid_ || scissor_rect != scissor_) {
    context->SetScissorRects(1, &scissor_rect, 0, 0);
    scissor_ = scissor_rect;
  }
  viewport_valid_ = true;
  if (state.stencil_enable) context->SetStencilRef(stencil_reference);
  const float blend_factors[4] = {regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_GREEN),
                                  regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_ALPHA)};
  context->SetBlendFactors(blend_factors);
  // The grass prototype (grass.h): the main view's terrain draws.
  if (terrain_ && output_sized && pixel_shader && render_target_count && state.depth_enable) {
    CaptureTerrain(regs, *vertex_shader, viewport, scissor_rect, state);
  }

  IBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  VALUE_TYPE index_type = primitives.host_index_format == xenos::IndexFormat::kInt16 ? VT_UINT16 : VT_UINT32;
  switch (primitives.index_buffer_type) {
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kNone:
      break;
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
      index_buffer = memory_->buffer();
      index_offset = primitives.guest_index_base;
      break;
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
      index_buffer = primitives_->converted_buffer();
      index_offset = primitives_->ConvertedOffset(primitives.host_index_buffer_handle);
      break;
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case rg::PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
      index_buffer = primitives_->builtin_buffer();
      index_offset = Primitives::BuiltinOffset(primitives.host_index_buffer_handle);
      break;
    default:
      ++stats_.failed;
      return false;
  }
  if (index_buffer) context->SetIndexBuffer(index_buffer, index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  auto issue = [&] {
    if (index_buffer) {
      DrawIndexedAttribs attribs{primitives.host_draw_vertex_count, index_type, DRAW_FLAG_NONE};
      context->DrawIndexed(attribs);
    } else {
      DrawAttribs attribs{primitives.host_draw_vertex_count, DRAW_FLAG_NONE};
      context->Draw(attribs);
    }
  };
  if (oversize) {
    // Stretched first (its black reaches the screen's edges), then in the box over it (its hole round).
    context->SetViewports(1, &stretched_viewport, 0, 0);
    context->SetScissorRects(1, &stretched_scissor, 0, 0);
    issue();
    context->SetViewports(1, &viewport, 0, 0);
    context->SetScissorRects(1, &scissor_rect, 0, 0);
  }
  issue();
  // D163: a model with lamps (a jeep's body, by its index count): its world matrix for the night's lights, in the
  // registers given with it (rigid rows, the position in their fourth floats); taken only when rigid.
  if (model_lamp_counts_ && model_lamp_report_ && index_buffer && output_sized && state.depth_write) {
    const uint32_t n = primitives.guest_draw_vertex_count;
    uint32_t first = UINT32_MAX;
    for (size_t k = 0; k + 1 < model_lamp_counts_->size(); k += 2)
      if ((*model_lamp_counts_)[k] == n) first = (*model_lamp_counts_)[k + 1];
    if (first <= 253) {
      float w[12];
      std::memcpy(w, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + first * 4], sizeof(w));
      static int dumped = 0;
      if (dumped < 2 && std::getenv("NXR_MODEL_LAMP_DUMP")) {
        ++dumped;
        std::string line;
        for (uint32_t r = 0; r < 40; ++r) {
          float q[4];
          std::memcpy(q, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + r * 4], sizeof(q));
          line += fmt::format(" c{} {:.3f} {:.3f} {:.3f} {:.3f};", r, q[0], q[1], q[2], q[3]);
        }
        std::string used;
        for (uint8_t r : vertex_registers_) used += fmt::format(" {}", r);
        REXLOG_INFO("[native_x_renderer] model lamps test: {} indices, VS registers used:{}; constants:{}", n, used, line);
      }
      bool rigid = true;
      for (int r = 0; r < 3 && rigid; ++r) {
        for (int q = 0; q < 3 && rigid; ++q) {
          const float d = w[r * 4] * w[q * 4] + w[r * 4 + 1] * w[q * 4 + 1] + w[r * 4 + 2] * w[q * 4 + 2];
          rigid = std::abs(d - (r == q ? 1.0f : 0.0f)) < 0.02f;
        }
      }
      if (rigid && std::abs(w[3]) < 1e5f && std::abs(w[7]) < 1e5f && std::abs(w[11]) < 1e5f) {
        Mat4d m{};
        for (int k = 0; k < 12; ++k) m[k] = w[k];
        m[15] = 1.0;
        model_lamp_report_(n, m);
      }
    }
  }

  // Object motion (D48): the main view's draws that write depth, matched with the last frame's; those that
  // moved are kept to draw their motion. A vertex shader that samples textures is left to the camera's
  // motion (the motion pipeline binds none). The camera vote (D118): each matched draw with a perspective matrix in
  // its constants offers this frame's and the last frame's.
  if (jittered && (object_motion_on_ || camera_vote_on_) && state.depth_write && !vertex_registers_.empty()) {
    struct {
      uint64_t vertex_shader;
      uint32_t vertex_address, index_address, vertex_count, index_type;
    } key{vertex_shader->ucode_data_hash(), first_vertex_address, regs[rg::XE_GPU_REG_VGT_DMA_BASE],
          primitives.guest_draw_vertex_count, uint32_t(primitives.index_buffer_type)};
    const uint8_t* matched = nullptr;
    uint32_t count = uint32_t(vertex_registers_.size());
    const uint8_t* previous = object_motion_.Match(XXH3_64bits(&key, sizeof(key)), vertex_constants_.data(),
                                                   vertex_registers_.data(), count, &matched);
    uint32_t matrix = 0;
    bool transposed = false;
    const Mat4d* reprojection = nullptr;
    if (camera_vote_on_ && matched &&
        CameraVote::FindMatrix(vertex_constants_.data(), vertex_registers_.data(), count, matrix, transposed)) {
      reprojection = camera_vote_.Offer(CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed),
                                        CameraVote::MatrixAt(matched, matrix, transposed));
    }
    if (object_motion_on_ && previous && vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
      // D120: a draw that moved with the camera only (each object's own world-view-projection changes with the camera:
      // GH Metallica's) needs no motion draw: its constants changed only in its matrix (and the view's shared ones, not
      // in bones), and its matrix's reprojection is the camera's. Against the vote's leader now; the others against its
      // winner when drawn.
      if (reprojection &&
          !object_motion_.ChangedOnlyIn(vertex_constants_.data(), previous, matrix, matrix + 4)) {
        reprojection = nullptr;
      }
      Mat4d leader;
      uint32_t votes = 0, offers = 0;
      if (reprojection && camera_vote_.Result(leader, votes, offers) && votes >= kLeaderVotes && votes * 2 >= offers &&
          CameraVote::WithinPx(*reprojection, leader, jitter_size_[0], jitter_size_[1], kCameraMotionPx)) {
        ++stats_.motion_camera;
      } else {
        KeepMotionDraw(*vertex_shader, vertex_modification.value, state, previous, viewport, scissor_rect,
                       index_buffer, index_offset, index_type, primitives.host_draw_vertex_count, reprojection);
      }
    }
  }
  return true;
}

bool DrawPath::EnsureMotionPixelShader() {
  if (motion_pixel_shader_) return true;
  if (motion_pixel_shader_failed_) return false;
  motion_pixel_shader_ = CreateMotionPixelShader(device_->device());
  if (!motion_pixel_shader_) {
    motion_pixel_shader_failed_ = true;
    REXLOG_ERROR("[native_x_renderer] object motion: the motion pixel shader could not be created");
    return false;
  }
  return true;
}

void DrawPath::KeepMotionDraw(rg::SpirvShader& vertex_shader, uint64_t vertex_modification, const PipelineState& state,
                              const uint8_t* previous_constants, const Viewport& viewport, const Rect& scissor,
                              IBuffer* index_buffer, uint64_t index_offset, VALUE_TYPE index_type,
                              uint32_t vertex_count, const Mat4d* reprojection) {
  if (!EnsureMotionPixelShader()) return;
  // Made on a worker thread once the draw's own vertex shader is ready; until then, and for a shader it
  // cannot be made for, the draw keeps the camera's motion.
  bool pending = false;
  IShader* motion_vertex_shader = shaders_->GetMotionShader(vertex_shader, vertex_modification, pending);
  if (!motion_vertex_shader) return;
  // The draw's state, into the motion vectors: depth tested equal to what the draw wrote, not written; no
  // stencil, no blending.
  PipelineState motion = state;
  motion.vertex_shader = motion_vertex_shader;
  motion.pixel_shader = motion_pixel_shader_.RawPtr();
  std::fill(std::begin(motion.rtv_formats), std::end(motion.rtv_formats), uint16_t(0));
  motion.rtv_formats[0] = uint16_t(kMotionVectorFormat);
  motion.render_target_count = 1;
  motion.depth_write = 0;
  motion.depth_func = uint8_t(COMPARISON_FUNC_EQUAL);
  motion.stencil_enable = 0;
  motion.stencil_read_mask = motion.stencil_write_mask = 0;
  std::memset(motion.stencil_front, 0, sizeof(motion.stencil_front));
  std::memset(motion.stencil_back, 0, sizeof(motion.stencil_back));
  std::memset(motion.targets, 0, sizeof(motion.targets));
  motion.targets[0].write_mask = uint8_t(COLOR_MASK_RED | COLOR_MASK_GREEN);
  bool created = false;
  Pipeline* pipeline = pipelines_->Get(motion, vertex_shader, nullptr, &created, true);
  if (created) {
    // Stored with the others (D137, before only in DiligentCore's pipeline cache): the motion state itself, marked by
    // its pixel shader hash.
    if (storage_) {
      storage_->AddPipeline({vertex_shader.ucode_data_hash(), vertex_modification, PipelineStorage::kMotionPixelHash, 0,
                             motion});
    }
    ++pipelines_unsaved_;
    frames_since_created_ = 0;
  }
  if (!pipeline || !pipelines_->Ready(*pipeline)) return;
  MotionDraw draw{pipeline, motion_data_.size(), uint32_t(vertex_constants_.size()), viewport, scissor,
                  previous_constants, index_buffer, index_offset, index_type, vertex_count,
                  reprojection != nullptr, reprojection ? *reprojection : Mat4d{}};
  const rg::RegisterFile& regs = *registers_;
  const auto* system = reinterpret_cast<const uint8_t*>(&system_constants_);
  const auto* bool_loop = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]);
  const auto* fetch = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0]);
  motion_data_.insert(motion_data_.end(), vertex_constants_.begin(), vertex_constants_.end());
  motion_data_.insert(motion_data_.end(), system, system + sizeof(system_constants_));
  motion_data_.insert(motion_data_.end(), bool_loop, bool_loop + kBoolLoopConstantsSize);
  motion_data_.insert(motion_data_.end(), fetch, fetch + kFetchConstantsSize);
  motion_draws_.push_back(draw);
}

uint32_t DrawPath::DrawObjectMotion(ITextureView* motion, ITextureView* depth) {
  const auto started = std::chrono::steady_clock::now();
  uint32_t drawn = 0;
  if (motion && depth && !motion_draws_.empty()) {
    IDeviceContext* context = device_->context();
    context->SetRenderTargets(1, &motion, depth, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    TargetsChanged();
    uploaded_frame_ = UINT64_MAX;  // its own bool/loop and fetch constants go into the draws' buffers (D81)
    memory_->PrepareForRead(context);
    for (const MotionDraw& draw : motion_draws_) {
      // D120: it moved with the camera only, the vote's winner.
      if (draw.has_reprojection && camera_winner_valid_ &&
          CameraVote::WithinPx(draw.reprojection, camera_winner_, jitter_size_[0], jitter_size_[1], kCameraMotionPx)) {
        ++stats_.motion_camera;
        continue;
      }
      const uint8_t* data = motion_data_.data() + draw.data;
      Pipeline& pipeline = *draw.pipeline;
      auto upload = [&](IBuffer* buffer, size_t size) {
        MappedBuffer map(context, buffer);
        std::memcpy(map.data(), data, size);
        data += size;
      };
      upload(pipeline.float_constants[0], draw.constants_size);
      {
        MappedBuffer map(context, pipeline.previous_float_constants);
        std::memcpy(map.data(), draw.previous_constants, draw.constants_size);
      }
      upload(system_constants_buffer_, sizeof(system_constants_));
      upload(bool_loop_constants_buffer_, kBoolLoopConstantsSize);
      upload(fetch_constants_buffer_, kFetchConstantsSize);
      {
        const float ndc_to_pixels[4] = {0.5f * draw.viewport.Width, -0.5f * draw.viewport.Height, 0.0f, 0.0f};
        MappedBuffer map(context, motion_constants_buffer_);
        std::memcpy(map.data(), ndc_to_pixels, sizeof(ndc_to_pixels));
      }
      context->SetPipelineState(pipeline.pso);
      context->CommitShaderResources(pipeline.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      context->SetViewports(1, &draw.viewport, 0, 0);
      context->SetScissorRects(1, &draw.scissor, 0, 0);
      if (draw.index_buffer) {
        context->SetIndexBuffer(draw.index_buffer, draw.index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DrawIndexedAttribs attribs{draw.vertex_count, draw.index_type, DRAW_FLAG_NONE};
        context->DrawIndexed(attribs);
      } else {
        DrawAttribs attribs{draw.vertex_count, DRAW_FLAG_NONE};
        context->Draw(attribs);
      }
      ++drawn;
    }
  }
  stats_.motion_drawn += drawn;
  stats_.motion_draw_ns +=
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
  motion_draws_.clear();
  motion_data_.clear();
  return drawn;
}

// VulkanCommandProcessor::UpdateSystemConstantValues, for the host render target path (no fragment shader
// interlock) at resolution scale 1. Fields that do not apply to a draw keep their last value, as there.
void DrawPath::FillSystemConstants(const DrawContext& draw) {
  const rg::RegisterFile& regs = *draw.regs;
  const auto& primitives = *draw.primitives;
  const auto& viewport = *draw.viewport;
  Translator::SystemConstants& c = system_constants_;
  using PIBT = rg::PrimitiveProcessor::ProcessedIndexBufferType;

  auto vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  uint32_t flags = 0;
  if (primitives.index_buffer_type == PIBT::kHostBuiltinForDMA || primitives.index_buffer_type == PIBT::kGuestDMA ||
      primitives.index_buffer_type == PIBT::kHostConverted) {
    flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    if (primitives.host_primitive_reset_enabled &&
        primitives.host_vertex_shader_type != rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitives.host_vertex_shader_type != rg::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (vte_cntl.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
  if (vte_cntl.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
  if (vte_cntl.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
  if (draw.polygonal) flags |= Translator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= Translator::kSysFlag_PrimitiveLine;
  flags |= uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= Translator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test =
      colorcontrol.alpha_test_enable ? colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
  float alpha_reference = regs.Get<float>(rg::XE_GPU_REG_RB_ALPHA_REF);
  if (draw.alpha_to_coverage_blended && draw.depth_write) {
    // D69: depth where the alpha is at least 0.5, or where the game's own test is stricter.
    if (alpha_test == xenos::CompareFunction::kAlways) {
      alpha_test = xenos::CompareFunction::kGreaterEqual;
      alpha_reference = 0.5f;
    } else if (alpha_test == xenos::CompareFunction::kGreater || alpha_test == xenos::CompareFunction::kGreaterEqual) {
      alpha_reference = std::max(alpha_reference, 0.5f);
    }
  }
  flags |= uint32_t(alpha_test) << Translator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma render targets are 8-bit here (targets.cpp): the shader writes gamma-encoded values.
  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  c.flags = flags;

  if (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad) {
    c.vertex_index_load_address = primitives.guest_index_base;
  }
  c.vertex_index_reset = (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
                             ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
                             : 0;
  c.compute_memexport_vertex_count = 0;
  // The endian the processed indices need (the SDK uses the guest's whenever the shader may load indices,
  // which differs only for pre-swapped converted indices).
  c.vertex_index_endian = primitives.host_shader_index_endian;
  c.line_loop_closing_index = primitives.line_loop_closing_index;
  c.vertex_base_index = regs.Get<int32_t>(rg::XE_GPU_REG_VGT_INDX_OFFSET);
  c.vertex_index_min = regs.Get<uint32_t>(rg::XE_GPU_REG_VGT_MIN_VTX_INDX);
  c.vertex_index_max = regs.Get<uint32_t>(rg::XE_GPU_REG_VGT_MAX_VTX_INDX);

  // User clip planes: none on this device (the modification says 0 planes).
  (void)clip_cntl;

  c.tessellation_factor_range_min = regs.Get<float>(rg::XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  c.tessellation_factor_range_max = regs.Get<float>(rg::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  for (uint32_t i = 0; i < 3; ++i) {
    c.ndc_scale[i] = viewport.ndc_scale[i];
    c.ndc_offset[i] = viewport.ndc_offset[i];
  }

  if (draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    c.point_vertex_diameter_min = float(point_minmax.min_size) * (2.0f / 16.0f);
    c.point_vertex_diameter_max = float(point_minmax.max_size) * (2.0f / 16.0f);
    c.point_constant_diameter[0] = float(point_size.width) * (2.0f / 16.0f);
    c.point_constant_diameter[1] = float(point_size.height) * (2.0f / 16.0f);
    c.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(std::max(viewport.xy_extent[0], uint32_t(1)));
    c.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(std::max(viewport.xy_extent[1], uint32_t(1)));
  }

  // Texture signs and swizzles (the swizzle is applied in the shader: the views are RGBA).
  uint32_t remaining = draw.used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(1u << index);
    uint32_t swizzle;
    uint8_t signs;
    textures_->HostSwizzle(index, regs.GetTextureFetch(index), swizzle, signs);
    uint32_t& signs_word = c.texture_swizzled_signs[index >> 2];
    uint32_t signs_shift = 8 * (index & 3);
    signs_word = (signs_word & ~(0xFFu << signs_shift)) | (uint32_t(signs) << signs_shift);
    uint32_t& swizzle_word = c.texture_swizzles[index >> 1];
    uint32_t swizzle_shift = 12 * (index & 1);
    swizzle_word = (swizzle_word & ~(0xFFFu << swizzle_shift)) | (swizzle << swizzle_shift);
  }
  // Resolve results are at the render scale: the shaders revert it in texel offsets and weights (D37).
  c.textures_resolution_scaled = 0;
  if (scale_.scaled()) {
    uint32_t used = draw.used_texture_mask;
    while (rex::bit_scan_forward(used, &index)) {
      used &= ~(1u << index);
      if (textures_->IsResolved(index, regs.GetTextureFetch(index))) c.textures_resolution_scaled |= 1u << index;
    }
  }

  c.alpha_test_reference = alpha_reference;
  c.alpha_to_mask = colorcontrol.alpha_to_mask_enable && !draw.alpha_to_coverage_blended
                        ? (colorcontrol.value >> 24) | (1u << 8)
                        : 0;

  // Color exponent bias. 16-bit fixed-point targets (k_16_16, k_16_16_16_16) are half floats here and hold the
  // guest's values as they are (D130): no extra bias. Targets in a host format wider than the guest's have their bias
  // given negative, which has the shader keep the color (the blending source) within the guest format's range in
  // edram_rt_clamp, NaN as 0, as the Xenos does (D139: Battlefield 1943's smoke and fire, very bright and flickering
  // late in a match; one fragment of an explosion wrote 70 into its 7e3 scene target, whose top is 31.875).
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t bias = color_infos[i].color_exp_bias;
    uint32_t bits = 0x3F800000u + (uint32_t(bias) << 23);
    float range[4];  // low color, low alpha, high color, high alpha
    if (GuestColorRange(uint32_t(color_infos[i].color_format), range)) {
      bits |= 0x80000000u;
      std::memcpy(c.edram_rt_clamp[i], range, sizeof(range));
    }
    std::memcpy(&c.color_exp_bias[i], &bits, sizeof(bits));
  }
}

uint32_t DrawPath::TakePackedRead() { return textures_->TakePackedRead(); }

// D154: the frames' cameras (position), the cameras their models were placed from (placed) and the moves between, the
// last 16, from the game's thread.
namespace {
struct AttachedMove {
  float position[3] = {};
  float placed[3] = {};
  Mat4d move{};
  std::chrono::steady_clock::time_point at{};
};
std::mutex g_attached_mutex;
std::array<AttachedMove, 16> g_attached{};
uint32_t g_attached_next = 0;
std::atomic<bool> g_attached_any{false};

// The record whose frame camera is the draw's eye (within 5 mm) and whose placed camera the model holds (in its own
// space, `world_inverse`: within half a metre, as a model placed from that camera does; D156).
bool FindAttachedMove(const double eye[3], const Mat4d& world_inverse, Mat4d& move) {
  std::lock_guard lock(g_attached_mutex);
  const auto now = std::chrono::steady_clock::now();
  double best = 0.005 * 0.005;  // within 5 mm
  bool found = false;
  for (const AttachedMove& m : g_attached) {
    if (now - m.at > std::chrono::milliseconds(500)) continue;
    double d2 = 0, o2 = 0;
    for (int c = 0; c < 3; ++c) d2 += (eye[c] - m.position[c]) * (eye[c] - m.position[c]);
    if (d2 > best) continue;
    for (int r = 0; r < 3; ++r) {
      const double v = world_inverse[r * 4] * m.placed[0] + world_inverse[r * 4 + 1] * m.placed[1] +
                       world_inverse[r * 4 + 2] * m.placed[2] + world_inverse[r * 4 + 3];
      o2 += v * v;
    }
    if (o2 <= 0.5 * 0.5) best = d2, move = m.move, found = true;
  }
  return found;
}
}  // namespace

void RecordAttachedCameraMove(const float* position, const float* move, const float* placed) {
  std::lock_guard lock(g_attached_mutex);
  AttachedMove& m = g_attached[g_attached_next++ % g_attached.size()];
  std::memcpy(m.position, position, sizeof(m.position));
  std::memcpy(m.placed, placed, sizeof(m.placed));
  for (int i = 0; i < 16; ++i) m.move[i] = move[i];
  m.at = std::chrono::steady_clock::now();
  g_attached_any.store(true, std::memory_order_relaxed);
}

void DrawPath::MoveAttached(const uint32_t* regs, uint32_t first, const uint64_t* float_bitmap, uint8_t* mapped) {
  if (!g_attached_any.load(std::memory_order_relaxed)) return;
  // The used registers in order: the mapped buffer holds them so, 16 bytes each.
  uint8_t numbers[256];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = float_bitmap[i];
    while (bits) {
      numbers[n++] = uint8_t((i << 6) + uint32_t(std::countr_zero(bits)));
      bits &= bits - 1;
    }
  }
  if (n < 7) return;
  std::array<uint8_t, 256 * 16> values;
  for (uint32_t k = 0; k < n; ++k) std::memcpy(values.data() + k * 16, &regs[first + (uint32_t(numbers[k]) << 2)], 16);
  uint32_t index = 0;
  bool transposed = false;
  if (!CameraVote::FindMatrix(values.data(), numbers, n, index, transposed)) return;
  const Mat4d a = CameraVote::MatrixAt(values.data(), index, transposed);
  GtaoProjection projection;
  if (!GtaoProjection::FromMatrix(a, projection) || !(projection.near_plane() > 0.0f) ||
      projection.near_plane() >= 0.5f) {
    return;
  }
  // The eye in the object's space: where clip x, y and w are 0.
  const double m3[3][3] = {{a[0], a[1], a[2]}, {a[4], a[5], a[6]}, {a[12], a[13], a[14]}};
  const double rhs[3] = {-a[3], -a[7], -a[15]};
  const double det = m3[0][0] * (m3[1][1] * m3[2][2] - m3[1][2] * m3[2][1]) -
                     m3[0][1] * (m3[1][0] * m3[2][2] - m3[1][2] * m3[2][0]) +
                     m3[0][2] * (m3[1][0] * m3[2][1] - m3[1][1] * m3[2][0]);
  if (std::abs(det) < 1e-12) return;
  double eye[3];
  for (int c = 0; c < 3; ++c) {
    double m[3][3];
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 3; ++k) m[r][k] = k == c ? rhs[r] : m3[r][k];
    eye[c] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
              m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) /
             det;
  }
  // Models near the eye (in their own units: a model may be drawn scaled); which of them were placed from a recorded
  // camera is decided with their world matrix below (D156: they hold that camera, however far the frame's is).
  if (eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2] > 25.0 * 25.0) return;
  ++attached_seen_;
  {  // every 10 s while any are seen: how many were moved
    static auto at = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - at > std::chrono::seconds(10)) {
      at = std::chrono::steady_clock::now();
      REXLOG_INFO("[native_x_renderer] D154: draws of models at the eye {}, moved onto the frame's camera {}",
                  attached_seen_, attached_moved_);
      attached_seen_ = attached_moved_ = 0;
    }
  }
  // A world matrix: three consecutive registers out of the projection's, rigid rows with the position in the fourth
  // floats, that put the eye at a recorded camera.
  for (uint32_t k = 0; k + 2 < n; ++k) {
    if (numbers[k + 2] != numbers[k] + 2) continue;
    if (k + 2 >= index && k <= index + 3) continue;
    float r[3][4];
    for (int i = 0; i < 3; ++i) std::memcpy(r[i], values.data() + (k + i) * 16, 16);
    double len[3];
    bool rigid = true;
    for (int i = 0; i < 3 && rigid; ++i) {
      len[i] = std::sqrt(double(r[i][0]) * r[i][0] + double(r[i][1]) * r[i][1] + double(r[i][2]) * r[i][2]);
      rigid = len[i] > 0.01 && len[i] < 100.0;
    }
    if (!rigid) continue;
    auto dot = [&](int i, int j) {
      return (double(r[i][0]) * r[j][0] + double(r[i][1]) * r[j][1] + double(r[i][2]) * r[j][2]) / (len[i] * len[j]);
    };
    if (std::abs(dot(0, 1)) > 0.05 || std::abs(dot(0, 2)) > 0.05 || std::abs(dot(1, 2)) > 0.05) continue;
    // As rows of world = W x position (column form), the eye in the world.
    double world_eye[3];
    for (int i = 0; i < 3; ++i) world_eye[i] = r[i][0] * eye[0] + r[i][1] * eye[1] + r[i][2] * eye[2] + r[i][3];
    const Mat4d w = {r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3],
                     r[2][0], r[2][1], r[2][2], r[2][3], 0.0,     0.0,     0.0,     1.0};
    Mat4d w_inverse;
    if (!Mat4dInvert(w, w_inverse)) return;
    Mat4d move;
    if (!FindAttachedMove(world_eye, w_inverse, move)) continue;
    const Mat4d w2 = Mat4dMultiply(move, w);
    const Mat4d a2 = Mat4dMultiply(Mat4dMultiply(a, w_inverse), w2);
    // Into the GPU's copy: the projection block as it was laid out, and the world matrix.
    float out[16];
    for (int rr = 0; rr < 4; ++rr)
      for (int c = 0; c < 4; ++c) out[transposed ? c * 4 + rr : rr * 4 + c] = float(a2[rr * 4 + c]);
    std::memcpy(mapped + size_t(index) * 16, out, sizeof(out));
    for (int i = 0; i < 3; ++i) {
      float row[4];
      for (int c = 0; c < 4; ++c) row[c] = float(w2[i * 4 + c]);
      std::memcpy(mapped + size_t(k + i) * 16, row, sizeof(row));
    }
    ++attached_moved_;
    return;
  }
}

uint32_t DrawPath::TickInterpolate(uint64_t key, const uint8_t* constants, const uint8_t* registers, uint32_t count,
                                   uint8_t* mapped, uint32_t from) {
  uint32_t index = 0;
  bool transposed = false;
  if (from >= count ||
      !CameraVote::FindMatrix(constants + size_t(from) * 16, registers + from, count - from, index, transposed)) {
    ++tick_why_[7];
    // NXR_TICK_TRACE=3: the vertex shaders of the main view's draws with no perspective-looking block, once each.
    static const bool trace3 = [] {
      const char* v = std::getenv("NXR_TICK_TRACE");
      return v && *v == '3';
    }();
    if (trace3) {
      static std::unordered_set<uint64_t> seen;
      if (seen.size() < 64 && seen.insert(tick_trace_shader_).second) {
        REXLOG_INFO("[native_x_renderer] tick trace: vertex shader {:016x} draws with no perspective block in its {} "
                    "registers (first c{})",
                    tick_trace_shader_, count, count ? registers[0] : 0);
        for (uint32_t r = 0; r < std::min(count, 8u); ++r) {
          float v[4];
          std::memcpy(v, constants + size_t(r) * 16, 16);
          REXLOG_INFO("[native_x_renderer] tick trace:   c{} = {:.5f} {:.5f} {:.5f} {:.5f}", registers[r], v[0], v[1], v[2],
                      v[3]);
        }
      }
    }
    return UINT32_MAX;
  }
  index += from;
  TickBlock(key, constants, index, transposed, mapped);
  return index;
}

void DrawPath::TickBlock(uint64_t key, const uint8_t* constants, uint32_t index, bool transposed, uint8_t* mapped) {
  float now[16];
  std::memcpy(now, constants + size_t(index) * 16, sizeof(now));
  // The objects drawn with this key (instances of one mesh: trees, rocks, fences). A draw is the object drawn with the
  // same matrix earlier this frame (another pass: the same output), else the closest of those drawn last frame (D48's
  // rule; the game culls on several threads, so their order changes from frame to frame), else a new one.
  std::vector<TickEntry>& list = tick_entries_[key];
  for (TickEntry& c : list) {
    if (c.frame == tick_frame_ && !std::memcmp(c.raw, now, sizeof(now))) {
      if (c.blended_now) std::memcpy(mapped + size_t(index) * 16, c.out, sizeof(c.out)), ++tick_blended_frame_;
      return;
    }
  }
  size_t best = list.size();
  float best_d = 0;
  for (size_t k = 0; k < list.size(); ++k) {
    const TickEntry& c = list[k];
    if (c.frame + 1 != tick_frame_) continue;  // drawn last frame and not taken yet this frame
    float d = 0;
    for (int i = 0; i < 16; ++i) d += std::abs(now[i] - c.cur[i]);
    if (best == list.size() || d < best_d) best = k, best_d = d;
  }
  if (best == list.size()) {
    // Reuse an entry not drawn for two seconds, else a new one.
    for (size_t k = 0; k < list.size() && best == list.size(); ++k)
      if (tick_frame_ - list[k].frame > 120) best = k;
    if (best == list.size()) list.emplace_back(), best = list.size() - 1;
    list[best] = TickEntry{};
  }
  TickEntry& e = list[best];
  const bool continued = e.frame && e.frame + 1 == tick_frame_;
  if (continued) e.phase += frame_seconds_;
  e.frame = tick_frame_;
  e.blended_now = false;
  std::memcpy(e.raw, now, sizeof(now));
  if (!continued) {
    e.have_prev = false, e.stepped = false, e.unchanged = 0, e.phase = 0;
    std::memcpy(e.cur, now, sizeof(now));
  } else if ([&] {
               // Changed beyond a ten thousandth of its largest entry (a drift that small on the frames between the
               // ticks, 0.0002 against tick steps of 0.04 in run vcam4, is not a step of its own).
               float d = 0, m = 1;
               for (int i = 0; i < 16; ++i) d = std::max(d, std::abs(now[i] - e.cur[i])), m = std::max(m, std::abs(now[i]));
               return d > 1e-4f * m;
             }()) {
    // NXR_TICK_TRACE=2: up to 8 draws a frame that changed two frames running, with their largest entry change.
    static const bool trace2 = [] {
      const char* v = std::getenv("NXR_TICK_TRACE");
      return v && *v == '2';
    }();
    if (trace2 && e.unchanged == 0) {
      if (tick_trace_frame_ != tick_frame_) tick_trace_frame_ = tick_frame_, tick_trace_count_ = 0;
      if (tick_trace_count_++ < 8) {
        float d = 0, m = 0;
        int at = 0;
        for (int i = 0; i < 16; ++i) {
          if (std::abs(now[i] - e.cur[i]) > d) d = std::abs(now[i] - e.cur[i]), at = i;
          m = std::max(m, std::abs(now[i]));
        }
        REXLOG_INFO("[native_x_renderer] tick trace: frame {} key {:016x} changed again: largest change {:.7f} at "
                    "entry {} (largest entry {:.3f}); block at register index {}",
                    tick_frame_, key, d, at, m, index);
      }
    }
    // A change: in the tick's rhythm when the matrix stayed for a frame or more before it (a camera that moves every
    // frame, on foot, changes the matrix every frame: not stepped, not touched).
    e.stepped = e.unchanged >= 1;
    std::memcpy(e.prev, e.cur, sizeof(now));
    std::memcpy(e.cur, now, sizeof(now));
    e.have_prev = true;
    e.unchanged = 0;
    constexpr double kTick = 1.0 / 30.0;
    e.phase = e.phase > 2 * kTick ? 0.0 : std::max(0.0, e.phase - kTick);  // a hitch: start again (look.cpp's rule)
  } else {
    ++e.unchanged;
  }
  // A draw that is not blended in a view that moves in the tick (a new one, or a jump): the camera's blend of the
  // tick, so it stays with its blended neighbours (run vcam5: unblended terrain left a grey wedge on a hill).
  auto with_camera = [&](uint32_t why) {
    ++tick_why_[why];
    if (tick_blended_last_ < 50 || tick_f_ < 0 || tick_f_ >= 1) return;
    const TickCandidate* best = nullptr;
    for (const TickCandidate& c : tick_candidates_)
      if (c.votes >= 2 && (!best || c.votes > best->votes)) best = &c;
    Mat4d d;
    if (best) {
      for (int i = 0; i < 16; ++i) d[i] = best->sum[i] / best->votes;
    } else if (tick_d_last_valid_) {
      d = tick_d_last_;
    } else {
      return;
    }
    Mat4d c;
    for (int i = 0; i < 16; ++i) c[i] = (1 - tick_f_) * d[i] + tick_f_ * ((i % 5) == 0 ? 1.0 : 0.0);
    const Mat4d m = Mat4dMultiply(c, CameraVote::MatrixAt(constants, index, transposed));
    float out[16];
    for (int r = 0; r < 4; ++r)
      for (int k = 0; k < 4; ++k) out[transposed ? k * 4 + r : r * 4 + k] = float(m[r * 4 + k]);
    std::memcpy(mapped + size_t(index) * 16, out, sizeof(out));
    std::memcpy(e.out, out, sizeof(out));  // a second pass of it this frame takes the same
    e.blended_now = true;
    ++tick_corrected_;
  };
  if (!e.have_prev) return with_camera(1);
  if (!e.stepped) return void(++tick_why_[2]);
  if (e.unchanged > 3) return void(++tick_why_[3]);
  if (frame_seconds_ <= 0 || frame_seconds_ > 0.025) return void(++tick_why_[4]);  // at 40 FPS and below: the frames
  const double f = std::clamp(e.phase * 30.0, 0.0, 1.0);
  if (f >= 1) return void(++tick_why_[5]);
  // A jump (a respawn, a teleport, a camera cut, or a value that is not a matrix's): not blended. Each entry may move by
  // a quarter of its own size, or of its row's largest entry (a rotation's entries pass zero).
  for (int r = 0; r < 4; ++r) {
    float row = 0;
    for (int c = 0; c < 4; ++c) row = std::max(row, std::max(std::abs(e.cur[r * 4 + c]), std::abs(e.prev[r * 4 + c])));
    for (int c = 0; c < 4; ++c) {
      const int i = r * 4 + c;
      const float d = std::abs(e.cur[i] - e.prev[i]);
      if (!(d <= 0.25f * std::max(std::abs(e.cur[i]), std::abs(e.prev[i])) + 0.1f * row)) return with_camera(6);
    }
  }
  for (int i = 0; i < 16; ++i) e.out[i] = float(e.prev[i] + (e.cur[i] - e.prev[i]) * f);
  e.blended_now = true;
  if (tick_f_ < 0) tick_f_ = f;
  // Its tick step in clip space offered to the camera's vote (up to 48 a frame): the static majority agree.
  if (tick_offers_ < 48) {
    ++tick_offers_;
    Mat4d prev, cur, inverse;
    for (int r = 0; r < 4; ++r)
      for (int k = 0; k < 4; ++k) {
        prev[r * 4 + k] = e.prev[transposed ? k * 4 + r : r * 4 + k];
        cur[r * 4 + k] = e.cur[transposed ? k * 4 + r : r * 4 + k];
      }
    if (Mat4dInvert(cur, inverse)) {
      const Mat4d d = Mat4dMultiply(prev, inverse);
      bool joined = false;
      for (TickCandidate& c : tick_candidates_) {
        double diff = 0;
        for (int i = 0; i < 16; ++i) diff = std::max(diff, std::abs(d[i] - c.first[i]));
        if (diff < 2e-3) {
          for (int i = 0; i < 16; ++i) c.sum[i] += d[i];
          ++c.votes;
          joined = true;
          break;
        }
      }
      if (!joined && tick_candidates_.size() < 8) {
        TickCandidate c;
        c.first = d;
        c.sum = d;
        c.votes = 1;
        tick_candidates_.push_back(c);
      }
    }
  }
  std::memcpy(mapped + size_t(index) * 16, e.out, sizeof(e.out));
  // NXR_TICK_TRACE=1: the first blended draw of each frame, the fraction and the last column.
  static const bool trace = [] {
    const char* v = std::getenv("NXR_TICK_TRACE");
    return v && *v == '1';
  }();
  if (trace && !tick_blended_frame_) {
    REXLOG_INFO("[native_x_renderer] tick trace: frame {} key {:016x} f {:.3f} dt {:.1f} ms out {:.4f} {:.4f} {:.4f} "
                "{:.4f} prev {:.4f} cur {:.4f}",
                tick_frame_, key, f, frame_seconds_ * 1000, e.out[3], e.out[7], e.out[11], e.out[15], e.prev[3],
                e.cur[3]);
  }
  ++tick_blended_frame_;
}

void DrawPath::EndFrame() {
  // D138: the frame's time, and the entries of objects not drawn for two seconds dropped.
  {
    const auto now = std::chrono::steady_clock::now();
    frame_seconds_ = last_end_frame_.time_since_epoch().count()
                         ? std::chrono::duration<double>(now - last_end_frame_).count()
                         : 0.0;
    last_end_frame_ = now;
    ++tick_frame_;
    {
      const TickCandidate* best = nullptr;
      for (const TickCandidate& c : tick_candidates_)
        if (c.votes >= 2 && (!best || c.votes > best->votes)) best = &c;
      if (best) {
        for (int i = 0; i < 16; ++i) tick_d_last_[i] = best->sum[i] / best->votes;
        tick_d_last_valid_ = true;
      }
      tick_candidates_.clear();
      tick_offers_ = 0;
      tick_f_ = -1;
      tick_blended_last_ = tick_blended_frame_;
    }
    tick_blended_ += tick_blended_frame_;
    tick_blended_frame_ = 0;
    if (tick_frame_ % 600 == 0) {
      size_t objects = 0;
      for (auto it = tick_entries_.begin(); it != tick_entries_.end();) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const TickEntry& t) { return tick_frame_ - t.frame > 120; }),
                   list.end());
        objects += list.size();
        it = list.empty() ? tick_entries_.erase(it) : std::next(it);
      }
      if (tick_interpolation_) {
        REXLOG_INFO("[native_x_renderer] tick interpolation (D138): {:.1f} draws a frame blended between ticks; {} "
                    "objects followed; per frame not blended: {:.1f} instances, {:.1f} new, {:.1f} changing every "
                    "frame, {:.1f} still, {:.1f} at a low frame rate, {:.1f} a tick late, {:.1f} jumps; {:.1f} given "
                    "the camera's blend; {:.1f} with no matrix found",
                    double(tick_blended_) / 600.0, objects, tick_why_[0] / 600.0, tick_why_[1] / 600.0,
                    tick_why_[2] / 600.0, tick_why_[3] / 600.0, tick_why_[4] / 600.0, tick_why_[5] / 600.0,
                    tick_why_[6] / 600.0, double(tick_corrected_) / 600.0, tick_why_[7] / 600.0);
        tick_corrected_ = 0;
        for (double& w : tick_why_) w = 0;
      }
      tick_blended_ = 0;
    }
  }
  // The stored pipelines (D42): created as their shaders get ready; new records written; the pipeline
  // cache saved, on a thread of its own (D45), once nothing new was compiled for 10 s (at 60 FPS), and at
  // most every 5 minutes (D52): new areas bring pipelines every few seconds, and each save writes tens of
  // MB. What is not saved yet is in the records, compiled again at the next start.
  PrepareStored();
  if (storage_) PublishStoredProgress();
  if (storage_) storage_->Flush();
  if (save_failed_.exchange(false)) {
    pipelines_unsaved_ = std::max(pipelines_unsaved_, 1u);  // try again in 600 frames
    frames_since_created_ = 0;
    frames_since_saved_ = UINT32_MAX / 2;
  }
  ++frames_since_saved_;
  if (pipelines_unsaved_ && ++frames_since_created_ >= 600 && frames_since_saved_ >= 18000 && stored_.empty() &&
      stored_compiling_.empty()) {
    SavePipelineCache();
    frames_since_saved_ = 0;
  }
  if (primitives_) primitives_->EndFrame();
  if (memory_) memory_->EndFrame();
  if (guest_textures_) guest_textures_->BeginFrame(device_->frames_presented() + 1, device_->frames_completed());
  if (textures_) textures_->BeginFrame();
  // Object motion (D48): what DLAA did not draw (no main view this frame) is dropped.
  object_motion_.EndFrame();
  motion_draws_.clear();
  motion_data_.clear();
  // Every 600 frames, the pipelines not drawn in the last 600 let go of their textures (D55), now that the
  // motion draws, which commit the bindings of the frame's draws again, are done.
  uint64_t frame = device_->frames_presented();
  // D111: a game making new texture views all the time would make new binding sets as fast, each holding its
  // textures: past 16384 sets, those not used in the last 8 frames are dropped at once (the frames in flight
  // release their descriptor sets in DiligentCore).
  if (pipelines_ && pipelines_->binding_set_count() > 16384) {
    uint32_t dropped = pipelines_->ReleaseSets(frame, 8);
    committed_ = nullptr;
    if (dropped && !many_sets_logged_) {
      many_sets_logged_ = true;
      REXLOG_WARN("[native_x_renderer] frame {}: over 16384 binding sets kept; {} not used in 8 frames dropped (D111, "
                  "logged once)",
                  frame, dropped);
    }
  }
  if (pipelines_ && frame % 600 == 0) {
    PipelineCache::Released released = pipelines_->ReleaseTextures(frame, 600);
    if (released.sets) committed_ = nullptr;  // the last committed set may be gone (D111)
    if (released.pipelines || released.sets) {
      REXLOG_INFO("[native_x_renderer] frame {}: {} pipelines not drawn in 600 frames let go of their textures, {} "
                  "binding sets not used in 600 frames dropped (D111); {} textures only they held freed ({} MB)",
                  frame, released.pipelines, released.sets, released.textures, released.bytes >> 20);
    }
  }
}

DrawPath::Stats DrawPath::TakeStats() {
  Stats stats = stats_;
  stats_ = {};
  frame_waited_ms_ = 0.0;
  if (memory_) {
    stats.unwatched_pages = memory_->unwatched_pages();
    stats.unwatched_copy_bytes = memory_->copy_committed_bytes();
  }
  if (pipelines_) {
    stats.bindings_set += pipelines_->TakeSetVariables();
    stats.binding_sets_kept = pipelines_->binding_set_count();
  }
  if (textures_) {
    Textures::Stats textures = textures_->TakeStats();
    stats.textures_resolved = textures.resolved;
    stats.textures_guest = textures.guest;
    stats.textures_other_dimension = textures.other_dimension;
    stats.textures_placeholder = textures.placeholders;
  }
  if (shaders_) {
    ShaderCache::Times times = shaders_->TakeTimes();
    stats.analyses = times.analysis;
    stats.translations = times.translation;
  }
  if (pipelines_) stats.pipelines = pipelines_->TakeTime();
  ObjectMotion::Stats motion = object_motion_.TakeStats();
  stats.motion_matched = motion.matched;
  stats.motion_moved = motion.moved;
  stats.motion_candidates = motion.candidates;
  stats.motion_summarized = motion.summarized;
  stats.motion_match_ns = motion.match_ns;
  stats.motion_longest_key = motion.longest_key;
  return stats;
}

size_t DrawPath::shader_count() const { return shaders_ ? shaders_->count() : 0; }
size_t DrawPath::pipeline_count() const { return pipelines_ ? pipelines_->count() : 0; }
uint64_t DrawPath::texture_loads() const { return guest_textures_ ? guest_textures_->stats().loads : 0; }
uint64_t DrawPath::texture_loaded_bytes() const { return guest_textures_ ? guest_textures_->stats().loaded_bytes : 0; }
uint64_t DrawPath::texture_loads_unchanged() const {
  return guest_textures_ ? guest_textures_->stats().unchanged : 0;
}
uint64_t DrawPath::textures_unsupported() const { return guest_textures_ ? guest_textures_->stats().unsupported : 0; }
uint64_t DrawPath::texture_count() const { return guest_textures_ ? guest_textures_->stats().textures : 0; }
uint64_t DrawPath::texture_host_bytes() const { return guest_textures_ ? guest_textures_->stats().host_bytes : 0; }

bool DrawPath::TakeProjection(GtaoProjection& out, float& z_min, float& z_max) {
  static uint32_t calls = 0;
  if (++calls % 1200 == 600) {
    for (size_t i = 0; i < projections_.size(); ++i) {
      const auto& [p, n] = projections_[i];
      REXLOG_INFO("[native_x_renderer] ambient occlusion's camera vote: {} draws: near {:.4f} far {:.1f} px {:.4f} py {:.4f} "
                  "cx {:.4f} cy {:.4f} depth range {:.4f} {:.4f}", n, p.near_plane(), p.far_plane(), p.px, p.py, p.cx,
                  p.cy, projection_ranges_[i].first, projection_ranges_[i].second);
    }
  }
  uint32_t best = 0;
  for (size_t i = 0; i < projections_.size(); ++i) {
    if (projections_[i].second > best) {
      best = projections_[i].second, out = projections_[i].first;
      z_min = projection_ranges_[i].first, z_max = projection_ranges_[i].second;
    }
  }
  projections_.clear();
  projection_ranges_.clear();
  return best >= 3;
}

// The grass prototype (grass.h): a terrain draw is one whose vertex shader's only texture is a 2D k_16_FLOAT texture
// of at least 1024 texels across (Battlefield 1943's heightmap, 2048 x 2048). The first one a frame is kept.
void DrawPath::CaptureTerrain(const rg::RegisterFile& regs, const rg::SpirvShader& vertex_shader,
                              const Viewport& viewport, const Rect& scissor, const PipelineState& state) {
  // One fetch constant (the translator may bind it twice: as a 2D array and as a 3D texture).
  const auto& bindings = vertex_shader.GetTextureBindingsAfterTranslation();
  if (bindings.empty()) return;
  const auto* chosen = &bindings[0];
  for (const auto& b : bindings) {
    if (b.fetch_constant != bindings[0].fetch_constant) return;
    if (b.dimension != xenos::FetchOpDimension::k3DOrStacked && b.dimension != xenos::FetchOpDimension::kCube) chosen = &b;
  }
  const auto& binding = *chosen;
  const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kTexture || (fetch.format != xenos::TextureFormat::k_16_FLOAT && fetch.format != xenos::TextureFormat::k_16) ||
      fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.size_2d.width + 1 < 1024) {
    return;
  }
  TerrainFrame& t = *terrain_;
  ++t.draws;
  const uint32_t* c = &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X];
  auto get = [&](uint32_t reg, float* out) { std::memcpy(out, &c[reg * 4], 16); };
  // Its depth partition (by the viewport's depth range).
  {
    uint32_t p = 0;
    while (p < t.partition_count && (t.partitions[p].viewport.MinDepth != viewport.MinDepth ||
                                     t.partitions[p].viewport.MaxDepth != viewport.MaxDepth)) {
      ++p;
    }
    if (p == t.partition_count && p < t.partitions.size()) {
      TerrainFrame::Partition& n = t.partitions[t.partition_count++];
      for (uint32_t r = 0; r < 4; ++r) get(12 + r, n.wvp[r]);
      n.viewport = viewport;
      n.scissor = scissor;
      for (uint32_t i = 0; i < 3; ++i) {
        n.ndc_scale[i] = system_constants_.ndc_scale[i];
        n.ndc_offset[i] = system_constants_.ndc_offset[i];
      }
      n.draws = 0;
    }
    if (p < t.partition_count) ++t.partitions[p].draws;
  }
  if (t.valid) return;
  for (uint32_t r = 0; r < 4; ++r) get(12 + r, t.wvp[r]);
  get(20, t.scale);
  get(21, t.offset);
  get(22, t.uv);
  for (uint32_t i = 0; i < 3; ++i) {
    t.ndc_scale[i] = system_constants_.ndc_scale[i];
    t.ndc_offset[i] = system_constants_.ndc_offset[i];
  }
  t.viewport = viewport;
  t.scissor = scissor;
  t.depth_func = state.depth_func;
  t.depth_clip = state.depth_clip != 0;
  t.height = textures_->View(binding.fetch_constant, fetch, binding.dimension, binding.is_signed);
  t.height_size = fetch.size_2d.width + 1;
  t.height_unorm = fetch.format == xenos::TextureFormat::k_16;
  t.vertex_shader = vertex_shader.ucode_data_hash();
  t.valid = t.height != nullptr;
  static uint32_t logged = 0;
  if (logged++ < 3)
    REXLOG_INFO("[native_x_renderer] grass prototype: terrain draw VS {:016X}, heightmap fetch {} ({} texels, view {})",
                t.vertex_shader, binding.fetch_constant, t.height_size, t.valid ? "found" : "missing");
}

// SetHudClassify: one boxed draw's place. Its inline vertices' first two floats (big endian) are a quad's corners in the
// quad's own space (Battlefield 1943: -1..1, run 12 Nov 20:17); the vertex shader places it with its constants. Tried
// in turn: each pair of consecutive used registers as rows (x' = r.(x, y, 0, 1), y' = (r+1).(x, y, 0, 1)), then each
// four as a column major matrix (x' = r.x x + (r+1).x y + (r+3).x). The first that puts every corner in clip space
// (within 1.5) is the draw's place. Unknown: boxed in the middle.
DrawPath::HudPlace DrawPath::ClassifyHudDraw(std::span<const Copy> copies, uint64_t vertex_shader) {
  HudPlace place;
  HudTally& t = hud_tally_;
  ++t.draws;
  const uint32_t initiator = registers_->values[rg::XE_GPU_REG_VGT_DRAW_INITIATOR];
  const uint32_t count = initiator >> 16;
  float cx[4] = {}, cy[4] = {};  // the quad's extent's corners
  bool have_quad = false;
  if (copies.size() == 1 && count && copies[0].data) {
    const uint32_t stride = copies[0].size / count;
    if (stride >= 8) {
      float lo_x = 1e30f, hi_x = -1e30f, lo_y = 1e30f, hi_y = -1e30f;
      for (uint32_t v = 0; v < count; ++v) {
        float xy[2];
        for (int i = 0; i < 2; ++i) {
          uint32_t word;
          std::memcpy(&word, copies[0].data + size_t(v) * stride + i * 4, 4);
          word = rex::byte_swap(word);
          std::memcpy(&xy[i], &word, 4);
        }
        if (!std::isfinite(xy[0]) || !std::isfinite(xy[1])) continue;
        lo_x = std::min(lo_x, xy[0]), hi_x = std::max(hi_x, xy[0]);
        lo_y = std::min(lo_y, xy[1]), hi_y = std::max(hi_y, xy[1]);
      }
      if (lo_x <= hi_x) {
        const float xs[4] = {lo_x, hi_x, lo_x, hi_x}, ys[4] = {lo_y, lo_y, hi_y, hi_y};
        std::memcpy(cx, xs, sizeof(cx)), std::memcpy(cy, ys, sizeof(cy));
        have_quad = true;
      }
    }
  }
  auto reg = [&](size_t i) {  // the i-th used register's four floats
    const float* f = reinterpret_cast<const float*>(vertex_constants_.data() + i * 16);
    return f;
  };
  const size_t used = std::min(vertex_registers_.size(), vertex_constants_.size() / 16);
  double lo = 1e30, hi = -1e30, y_lo = 1e30, y_hi = -1e30;  // the placed quad's extent in clip space
  enum class How { kNone, kRows, kColumns } how = How::kNone;  // worded only for the debug log (once a draw before)
  uint32_t how_register = 0;
  auto try_place = [&](auto&& fx, auto&& fy) {
    double l = 1e30, h = -1e30, yl = 1e30, yh = -1e30;
    for (int k = 0; k < 4; ++k) {
      const double x = fx(cx[k], cy[k]), y = fy(cx[k], cy[k]);
      if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 6.0 || std::fabs(y) > 6.0) return false;
      l = std::min(l, x), h = std::max(h, x), yl = std::min(yl, y), yh = std::max(yh, y);
    }
    // A quad of some size placed by it (a row with no x or y term, such as (0, 0, 1, 0), puts every corner at one
    // point: not a transform, 12 Nov 20:40's text batches).
    if (h - l < 1e-4 || yh - yl < 1e-4) return false;
    lo = l, hi = h, y_lo = yl, y_hi = yh;
    return true;
  };
  if (have_quad) {
    for (size_t i = 0; i + 1 < used && how == How::kNone; ++i) {
      if (vertex_registers_[i + 1] != vertex_registers_[i] + 1) continue;
      const float *a = reg(i), *b = reg(i + 1);
      if (try_place([&](float x, float y) { return double(a[0]) * x + a[1] * y + a[3]; },
                    [&](float x, float y) { return double(b[0]) * x + b[1] * y + b[3]; }))
        how = How::kRows, how_register = vertex_registers_[i];
    }
    for (size_t i = 0; i + 3 < used && how == How::kNone; ++i) {
      if (vertex_registers_[i + 3] != vertex_registers_[i] + 3) continue;
      const float *a = reg(i), *b = reg(i + 1), *d = reg(i + 3);
      if (try_place([&](float x, float y) { return double(a[0]) * x + b[0] * y + d[0]; },
                    [&](float x, float y) { return double(a[1]) * x + b[1] * y + d[1]; }))
        how = How::kColumns, how_register = vertex_registers_[i];
    }
  }
  const char* result = "middle";
  HudPrev& prev = hud_prev_;
  // A mesh from vertex buffers with a 3D matrix (its third row not the 2D layer's (0, 0, 1, 0)): an overlay projected by
  // the game's camera, which the ultrawide hook already widened (12 Nov 21:10: the sniper scope's black mask, drawn
  // after the composite, boxed as "unknown"): drawn as the 3D view is, not boxed.
  bool mesh_3d = false;
  if (!have_quad && used >= 3) {
    const float* r2 = reg(2);
    mesh_3d = std::fabs(r2[0]) > 1e-4f || std::fabs(r2[1]) > 1e-4f || std::fabs(r2[3]) > 1e-3f;
  }
  if (how == How::kNone && mesh_3d) {
    ++t.stretched, result = "3D mesh, as the view";
    place.mode = HudMode::kStretch;
    hud_mesh_ = true;
  } else if (how == How::kNone) {
    // Not placed by a transform we know (a text batch): as the draw before it in this frame, the rest of its widget.
    ++t.unknown, result = "unknown";
    if (prev.valid && prev.place.mode == HudMode::kBox) place = prev.place, result = "unknown, as the last";
  } else if (lo < -1.02 && hi > 1.02) {
    // Past both edges (a scope's mask: a round hole in black larger than the screen): drawn twice, stretched to reach
    // the screen's edges, then in the middle so its hole stays round (DrawInternal).
    ++t.stretched, result = "oversize, twice";
    place.mode = HudMode::kOversize;
  } else if (hi - lo >= 1.8) {  // across the whole width (clip space is 2 wide)
    ++t.stretched, result = "stretched";
    place.mode = HudMode::kStretch;
  } else if (lo <= -0.97 && hi - lo >= 0.15) {  // from the canvas's left edge: from the screen's (not a small icon)
    ++t.left, result = "to the left edge";
    place.mode = HudMode::kExtendLeft;
  } else if (hi >= 0.97 && hi - lo >= 0.15) {
    ++t.right, result = "to the right edge";
    place.mode = HudMode::kExtendRight;
  } else if (const HudPanel* panel = [&]() -> const HudPanel* {
               for (auto it = hud_panels_.rbegin(); it != hud_panels_.rend(); ++it)
                 if (lo >= it->lo - 0.005 && hi <= it->hi + 0.005 && y_lo >= it->y_lo - 0.005 && y_hi <= it->y_hi + 0.005)
                   return &*it;
               return nullptr;
             }()) {
    // Inside a panel drawn before it: where the panel went.
    place = panel->place, result = "in a panel";
    ++(place.align == 0.0f ? t.left : place.align == 1.0f ? t.right : t.middle);
  } else if (double marker_x = -1; !hud_menu_ && NearHudMarker(((lo + hi) * 0.5 + 1.0) * 640.0,
                                                               (1.0 - (y_lo + y_hi) * 0.5) * 360.0, 48.0, &marker_x)) {
    // A world marker (the game's projection noted it, NoteHudMarker): its centre where the 3D view shows that place,
    // its size the HUD's. Every piece of one marker moves by the marker's own place, not by its own centre (13 Nov:
    // a plane's gunsight, drawn in several pieces around one projected point, came apart and shifted in flight).
    ++t.middle, ++t.markers, result = "world marker";
    place.mode = HudMode::kPoint;
    place.point_x = marker_x;
  } else if (prev.valid && prev.place.mode == HudMode::kBox && prev.vertex_shader == vertex_shader &&
             y_lo <= prev.y_hi && y_hi >= prev.y_lo && lo >= prev.lo - 0.02 && lo <= prev.hi + 0.15) {
    // The next glyph of a line, the next part of a bar: where the first went.
    place = prev.place, result = "as the last";
    ++(place.align == 0.0f ? t.left : place.align == 1.0f ? t.right : t.middle);
  } else if (hud_edges_ && !hud_menu_ && (lo + hi) * 0.5 <= -0.4) {
    ++t.left, result = "left";
    place.align = 0.0f;
  } else if (hud_edges_ && !hud_menu_ && (lo + hi) * 0.5 >= 0.4) {
    ++t.right, result = "right";
    place.align = 1.0f;
  } else {
    ++t.middle;
  }
  if (how != How::kNone) {
    if (lo <= -0.99 && hi >= 0.99 && y_lo <= -0.99 && y_hi >= 0.99 && hi <= 1.01 && lo >= -1.01) hud_backdrop_ = true;
    place.lo = lo, place.hi = hi, place.y_lo = y_lo, place.y_hi = y_hi;
    prev = {true, vertex_shader, lo, hi, y_lo, y_hi, place};
    if (place.mode == HudMode::kBox && y_hi - y_lo < 1.9) hud_area_ += (hi - lo) * (y_hi - y_lo);
    // Only a panel anchored to an edge passes its place on (a minimap's icons stay in it as it turns): a large
    // container in the middle took every widget into the middle with it (12 Nov 21:30, unzoomed).
    if (place.mode == HudMode::kBox && place.align != 0.5f && hi - lo >= 0.12 && y_hi - y_lo >= 0.2 &&
        y_hi - y_lo < 1.9)
      hud_panels_.push_back({lo, hi, y_lo, y_hi, place});
    // Moved since the last frame at the same order and size: a world marker for the next 3 s (90 frames at 30).
    const size_t index = hud_seen_.size();
    const HudSeen seen{(lo + hi) * 0.5, (y_lo + y_hi) * 0.5, hi - lo, y_hi - y_lo};
    if (index < hud_seen_last_.size()) {
      const HudSeen& last = hud_seen_last_[index];
      const bool same_size = std::fabs(last.w - seen.w) < 0.002 && std::fabs(last.h - seen.h) < 0.002;
      const bool moved = std::fabs(last.cx - seen.cx) > 0.003 || std::fabs(last.cy - seen.cy) > 0.003;
      if (hud_moved_frames_.size() <= index) hud_moved_frames_.resize(index + 1, 0);
      uint8_t& run = hud_moved_frames_[index];
      // Three frames in a row: a line added to the HUD shifts the draws after it once, a marker moves on and on.
      run = same_size && moved && std::string_view(result) != "in a panel" ? uint8_t(std::min(run + 1, 3)) : 0;
      if (run >= 3) {
        if (hud_moving_until_.size() <= index) hud_moving_until_.resize(index + 1, 0);
        hud_moving_until_[index] = hud_frame_ + 180;
      }
    }
    if (hud_seen_.size() < 4096) hud_seen_.push_back(seen);
  }
  if (hud_debug_ && t.sampled < 12) {
    ++t.sampled;
    std::string regs_text;
    for (size_t i = 0; i < std::min<size_t>(used, 6); ++i) {
      const float* f = reg(i);
      regs_text += fmt::format(" c{}({:.3f} {:.3f} {:.3f} {:.3f})", vertex_registers_[i], f[0], f[1], f[2], f[3]);
    }
    t.samples += fmt::format(" [quad {} x {:.2f}..{:.2f} y {:.2f}..{:.2f}; {}: x {:.2f}..{:.2f} -> {}{};{}]",
                             have_quad ? count : 0, cx[0], cx[1], cy[0], cy[2],
                             how == How::kNone ? std::string("none")
                                               : fmt::format("{} c{}", how == How::kRows ? "rows" : "columns", how_register),
                             lo, hi, result,
                             hud_menu_ ? " (menu)" : "", regs_text);
  }
  return place;
}

std::string DrawPath::TakeHudLog() {
  HudTally& t = hud_tally_;
  std::string s = fmt::format("{} draws ({} stretched full width, {} left, {} right, {} middle, {} place unknown; {} "
                              "world markers; {} menu frames);{}",
                              t.draws, t.stretched, t.left, t.right, t.middle, t.unknown, t.markers, t.menu_frames,
                              t.samples);
  t = {};
  return s;
}

}  // namespace native_x_renderer
