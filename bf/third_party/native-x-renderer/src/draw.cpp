// NativeXRenderer - see draw.h.

#include "draw.h"

#include <set>
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
#include "weapon_export.h"
#include "weapon_skin.h"

#include <rex/bit.h>
#include <rex/types.h>
#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <cstdlib>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <set>
#include <utility>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <xxhash.h>

REXCVAR_DEFINE_INT32(native_x_renderer_draw_log, 0, "NativeXRenderer",
                     "Bring-up: log the first N draws NativeXRenderer processes (primitive, indices, shaders)");
REXCVAR_DECLARE(int32_t, anisotropic_override);  // the SDK's: forced anisotropic filtering (textures.cpp)
REXCVAR_DEFINE_STRING(native_x_renderer_alpha_to_coverage, "auto", "NativeXRenderer",
                      "Alpha to coverage on targets without MSAA (D69, D114): auto (blend on surfaces the game draws "
                      "with MSAA, dither on those it draws at 1x), blend (the color blended by its alpha, depth written "
                      "where the alpha is at least 0.5: close to the game's MSAA) or dither (the shader's 2x2 pattern, "
                      "as on the console without MSAA)");

REXCVAR_DEFINE_BOOL(native_x_renderer_keep_pipeline_cache, false, "NativeXRenderer",
                    "Keep DiligentCore's pipeline cache for the whole run (D173). Off: it is let go once the stored "
                    "pipelines are compiled and the cache is saved (the driver's binaries of every stored pipeline, "
                    "tens of MB of system memory); the pipelines new in play are then compiled without it");
REXCVAR_DEFINE_BOOL(native_x_renderer_sea_game_light, true, "NativeXRenderer",
                    "D245: the sea lit as the game lights it (its sun's direction, its sky's, ground's and sun's colours, "
                    "its own painted sky reflected), only as bright as the hour");
REXCVAR_DEFINE_BOOL(native_x_renderer_ground_relief, true, "NativeXRenderer",
                    "D241: the ground's relief (tyre ruts, footprints, ripples) lit by a lower sun while the day and "
                    "night's stands higher than the game's own, as bright on flat ground");
REXCVAR_DEFINE_BOOL(native_x_renderer_ground_relief_game_sun, true, "NativeXRenderer",
                    "D241: the ground's relief lit from the game's own sun (its direction too) while the day and night's "
                    "stands higher, blended in over 15 degrees above the game's height; the brightness stays the hour's");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_ground_relief_game_sun_weight, 1.0, "NativeXRenderer",
                      "D244: with the game's sun, how far toward its direction the ground patches and tyre tracks are lit "
                      "(0 the hour's, 1 the game's)");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_ground_relief_terrain_weight, 1.0, "NativeXRenderer",
                      "D244: with the game's sun, how far toward its direction the terrain itself (its hills too) is lit");
REXCVAR_DEFINE_DOUBLE(native_x_renderer_ground_relief_strength, 0.5, "NativeXRenderer",
                      "D241: how far down toward the game's own sun the ground's relief is lit (0 the sky's sun, 1 the "
                      "game's: the ruts as the game shows them, but the slopes turned from the sun much darker)");
REXCVAR_DEFINE_BOOL(native_x_renderer_sun_source, true, "NativeXRenderer",
                    "D239: the sky's sun given to the game's own shadow setup (its cascades and what casts shadows follow "
                    "it) instead of turning its shadows afterwards");
REXCVAR_DEFINE_BOOL(native_x_renderer_tick_interpolation, false, "NativeXRenderer",
                    "Objects that move in the game's 30 Hz tick drawn between their last two ticks' positions, by the "
                    "time since the last one (D138): in a vehicle, where the camera moves only in the tick, the vehicle "
                    "and the world move smoothly above 30 FPS, one tick behind. Off: a draw that cannot be matched is drawn "
                    "with the tick's camera while its neighbours are blended, and gaps show (run vcam5)");

namespace native_x_renderer {
bool WeaponLayerMoveNow(double out[16]);  // (below, with RecordWeaponLayerMove)
void NoteFirstPersonWeaponDrawn();        // sky.cpp: the sea splashes only on a view that holds a weapon

// A developer's: the next draws logged (renderer.cpp names the moment).
std::atomic<int> g_dump_next_draws{0};
void DumpNextDraws(int count) { g_dump_next_draws.store(count, std::memory_order_relaxed); }

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

// A dynamic buffer's new contents (new memory each time), written until the end of the scope and recorded then.
void Upload(IDeviceContext* context, IBuffer* buffer, uint64_t size, const uint8_t* data) {
  void* mapped = nullptr;
  context->MapBuffer(buffer, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
  if (!mapped) return;
  std::memcpy(mapped, data, size);
  context->UnmapBuffer(buffer, MAP_WRITE);
}

class MappedBuffer {
 public:
  MappedBuffer(Recorder& recorder, IBuffer* buffer, BufferUploads* pending = nullptr)
      : recorder_(recorder), buffer_(buffer), size_(buffer->GetDesc().Size), data_(recorder.Bytes(size_)),
        pending_(pending) {}
  ~MappedBuffer() {
    if (pending_) {
      pending_->list[pending_->count++] = {buffer_, size_, data_};
      return;
    }
    recorder_.Do([buffer = buffer_, size = size_, data = data_](IDeviceContext* context) {
      Upload(context, buffer, size, data);
    });
  }
  uint8_t* data() const { return data_; }

 private:
  Recorder& recorder_;
  IBuffer* buffer_;
  uint64_t size_;
  uint8_t* data_;
  BufferUploads* pending_;
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

// A skinned draw of the first person weapon, kept for DrawSkinOver (weapon_skin.h).
struct DrawPath::SkinDraw {
  Pipeline* pipeline;
  size_t data;  // in skin_data_: the vertex shader's float constants, then the system, bool/loop and fetch constants
  uint32_t constants_size;
  Viewport viewport;
  Rect scissor;
  IBuffer* index_buffer;  // null: not indexed
  uint64_t index_offset;
  VALUE_TYPE index_type;
  uint32_t vertex_count;
  RefCntAutoPtr<ITextureView> texture;  // the skinned copy, as a plain 2D texture
  float width, height;
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
  Recorder* recorder = &device.recorder();

  memory_ = std::make_unique<GuestMemory>(memory, render_device, recorder);
  if (!memory_->Initialize()) return false;
  // Resolved textures are invalidated when their guest memory changes, which the mirror's watches report (D70).
  resolved_ = &resolved;
  resolved.Watch(memory_.get());
  primitives_ = std::make_unique<Primitives>(registers, memory, *memory_, render_device, recorder);
  if (!primitives_->Initialize()) {
    REXLOG_ERROR("[native_x_renderer] could not initialize primitive processing");
    return false;
  }
  shaders_ = std::make_unique<ShaderCache>();
  shaders_->SetSoftShadowFetchConstant(soft_shadow_fetch_constant_);  // D197
  if (!shaders_->Initialize(render_device, scale_)) return false;
  guest_textures_ = std::make_unique<GuestTextures>(registers, *memory_, resolved, render_device, recorder);
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
  // (64 bytes: the motion pass's one vec4, and after it the weapon skin's mesh pass's three, weapon_skin.cpp kOverPS.)
  motion_constants_buffer_ = CreateConstantBuffer(render_device, "NativeXRenderer motion constants", 64);
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

// HideCrosshair (native_x_renderer.h): until when the crosshair sheet's HUD draws are left out.
namespace {
std::atomic<int64_t> g_hide_crosshair_until{0};  // steady clock ticks
}  // namespace
void RecordHideCrosshair() {
  g_hide_crosshair_until.store((std::chrono::steady_clock::now() + std::chrono::milliseconds(250)).time_since_epoch().count(),
                               std::memory_order_relaxed);
}
namespace {
// D168: the game's scoped picture by its shaders (the guest programs' hashes; one game, so one set), Battlefield 1943's
// with a sniper scope up: its composite of the zoomed view, which blacks out all but the round view (962547C8BB566F8B
// with 8DD620FF8D8BB8FA: every steady scoped frame, run v41scope16, 13 Nov 2026; the reticle comes later with the HUD),
// and the full screen pass of the scope's raise, after two at half size (589877255C25EA63: only while the scope comes
// up, runs v41scope5 to v41scope12). The same vertex shader draws the luminance chain's small targets, so only a draw of
// the output's size counts. NXR_SCOPE_MASK=<vs>:<ps>,... adds pairs (hex; research).
bool IsScopeMask(uint64_t vs, uint64_t ps) {
  struct Pair {
    uint64_t vs, ps;
  };
  static const std::vector<Pair> pairs = [] {
    std::vector<Pair> out = {{0x962547C8BB566F8Bull, 0x8DD620FF8D8BB8FAull}, {0x962547C8BB566F8Bull, 0x589877255C25EA63ull}};
    if (const char* v = std::getenv("NXR_SCOPE_MASK"); v && *v) {
      const std::string text = v;
      for (size_t pos = 0; pos < text.size();) {
        const size_t comma = std::min(text.find(',', pos), text.size());
        const std::string item = text.substr(pos, comma - pos);
        const size_t colon = item.find(':');
        Pair p{std::strtoull(item.substr(0, colon).c_str(), nullptr, 16),
               colon == std::string::npos ? 0 : std::strtoull(item.substr(colon + 1).c_str(), nullptr, 16)};
        if (p.vs) out.push_back(p);
        pos = comma + 1;
      }
      REXLOG_INFO("[native_x_renderer] scope: {} mask shader pairs with NXR_SCOPE_MASK's", out.size());
    }
    return out;
  }();
  for (const Pair& p : pairs)
    if (p.vs == vs && (p.ps == 0 || p.ps == ps)) return true;
  return false;
}

// The x and y extent of an inline draw's vertices (their first two floats, big-endian): lo x, hi x, lo y, hi y.
bool InlineExtent(std::span<const DrawPath::Copy> copies, uint32_t count, float out[4]) {
  if (!count || copies.size() != 1 || !copies[0].data || copies[0].size < count * 8) return false;
  const uint32_t stride = copies[0].size / count;
  out[0] = out[2] = 1e30f, out[1] = out[3] = -1e30f;
  for (uint32_t v = 0; v < count; ++v) {
    float xy[2];
    for (int i = 0; i < 2; ++i) {
      uint32_t word;
      std::memcpy(&word, copies[0].data + size_t(v) * stride + i * 4, 4);
      word = rex::byte_swap(word);
      std::memcpy(&xy[i], &word, 4);
    }
    out[0] = std::min(out[0], xy[0]), out[1] = std::max(out[1], xy[0]);
    out[2] = std::min(out[2], xy[1]), out[3] = std::max(out[3], xy[1]);
  }
  return out[0] <= out[1];
}

// D249: every distinct draw of the game's that one of our own rules leaves out (the own sky's, the crosshair's), logged
// once: the rule, its pixel and vertex shaders, vertex count and target size, so that a rule that takes more than it
// was meant to shows in any log (D243: the sky's took every map's tyre tracks). tools/skip_audit.py names each pixel
// shader's samplers and constants from the game's own constant tables and flags those that are not the sky's.
// The renderer's thread only; at most 300 lines a run.
void NoteRuleSkip(const char* rule, uint64_t ps, uint64_t vs, uint32_t vertices, uint32_t width, uint32_t height,
                  uint32_t since_dome) {
  static std::set<std::tuple<const char*, uint64_t, uint64_t, uint32_t>> seen;
  if (seen.size() >= 300 || !seen.insert({rule, ps, vs, vertices}).second) return;
  REXLOG_INFO("[native_x_renderer] rule skip: {}: PS {:016X} VS {:016X}, {} vertices, target {}x{}, {} draws after the "
              "sky dome (D249)",
              rule, ps, vs, vertices, width, height, int32_t(since_dome));
}

}  // namespace

bool DrawPath::DrawInternal(const StateTracker::Program& vertex_program, const StateTracker::Program& pixel_program,
                            std::span<const Copy> copies, const std::array<HostTarget*, 5>& targets) {
  const rg::RegisterFile& regs = *registers_;
  Recorder& recorder = device_->recorder();
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
  // D168: while the picture in picture scope watches (SetScopeFrame), the game's own scope mask pass is noticed (the
  // scope's pass follows it); with the research log, each draw after the composite is named.
  if (scope_watch_ || scope_log_) {
    const HostTarget* target = nullptr;
    for (HostTarget* t : targets)
      if (t) {
        target = t;
        break;
      }
    if (scope_log_ && ++scope_logged_ <= 400) {
      float e[4];
      const bool extent = InlineExtent(copies, regs[rg::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16, e);
      REXLOG_INFO("[native_x_renderer] scope research, {}: vs {:016X} ps {:016X}, {} vertices{}, target {}x{} ({}), depth "
                  "{}{}, blend {:08X}{}",
                  scope_log_ == 2 ? "scoped" : scope_log_ == 3 ? "after the present" : "not scoped",
                  vertex_program.hash, pixel_program.hash, regs[rg::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16,
                  copies.empty() ? "" : " inline", target ? target->guest_width : 0, target ? target->guest_height : 0,
                  fmt::ptr(target), regs.Get<reg::RB_DEPTHCONTROL>().z_enable ? "tested" : "off",
                  regs.Get<reg::RB_DEPTHCONTROL>().z_write_enable ? " written" : "",
                  regs[rg::XE_GPU_REG_RB_BLENDCONTROL0],
                  extent ? fmt::format(", x {:.1f}..{:.1f} y {:.1f}..{:.1f}", e[0], e[1], e[2], e[3]) : "");
    }
    if (scope_watch_ && target && target->guest_width == jitter_size_[0] && target->guest_height == jitter_size_[1] &&
        IsScopeMask(vertex_program.hash, pixel_program.hash))
      scope_mask_drawn_ = true;
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
    // Bring-up aids (environment, read once): NXR_DRAWS_AFTER_S=<s> logs 2600 draws (two frames of a busy view) from that many seconds after the
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
    // The draws that follow a moment the renderer names (DumpNextDraws): the same line for each.
    if (g_dump_next_draws.load(std::memory_order_relaxed) > 0) {
      const int left = g_dump_next_draws.fetch_sub(1, std::memory_order_relaxed);
      REXLOG_INFO("[native_x_renderer] next draw {}: prim {} count {} index type {}; VS {:016X} PS {:016X}; surface "
                  "{:08X} color {:08X} depth {:08X} modecontrol {:08X} depthcontrol {:08X} blend {:08X}",
                  left, uint32_t(primitives.guest_primitive_type), primitives.guest_draw_vertex_count,
                  uint32_t(primitives.index_buffer_type), vs_hash, pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                  regs[0x2000], regs[0x2001], regs[0x2002], regs[0x2208], regs[0x2200], regs[0x2201]);
    }
    if (after_s >= 0) {
      static const auto first = std::chrono::steady_clock::now();
      static int dumped = 0;
      if (dumped < 2600 && std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() > after_s) {
        ++dumped;
        float vp[4];
        std::memcpy(vp, &regs[0x210F], sizeof(vp));
        REXLOG_INFO("[native_x_renderer] draw dump {}: prim {} count {} index type {}; VS {:016X} PS {:016X}; surface "
                    "{:08X} color {:08X} depth {:08X} modecontrol {:08X} depthcontrol {:08X}; scissor {:08X} {:08X}; "
                    "viewport scale {:.1f} {:.1f} offset {:.1f} {:.1f}",
                    dumped, uint32_t(primitives.guest_primitive_type), primitives.guest_draw_vertex_count,
                    uint32_t(primitives.index_buffer_type), vs_hash, pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                    regs[0x2000], regs[0x2001], regs[0x2002], regs[0x2208], regs[0x2200], regs[0x2081], regs[0x2082],
                    vp[0], vp[2], vp[1], vp[3]);
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
    // NXR_PS_LIST=<seconds> (a diagnosis aid): every pixel shader drawn in one frame that many seconds after the first
    // draw, with its draw count, logged once.
    if (pixel_shader) {
      static const double list_at = [] {
        const char* v = std::getenv("NXR_PS_LIST");
        return v && *v ? std::atof(v) : -1.0;
      }();
      if (list_at >= 0) {
        static const auto since = std::chrono::steady_clock::now();
        static uint64_t list_frame = 0;
        static bool listed = false;
        static std::vector<std::pair<uint64_t, int>> seen;
        const uint64_t now_frame = device_->frames_presented();
        if (!listed && std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count() >= list_at) {
          if (!list_frame) list_frame = now_frame + 1;
          if (now_frame == list_frame) {
            const uint64_t h = pixel_shader->ucode_data_hash();
            auto it = std::find_if(seen.begin(), seen.end(), [h](const auto& s) { return s.first == h; });
            if (it == seen.end()) seen.push_back({h, 1});
            else ++it->second;
          } else if (now_frame > list_frame) {
            listed = true;
            std::string out;
            for (const auto& [h, n] : seen) {
              out += fmt::format(" {:016X}x{}", h, n);
            }
            REXLOG_INFO("[native_x_renderer] pixel shaders of frame {} ({}):{}", list_frame,
                        seen.size(), out);
          }
        }
      }
    }
    // NXR_DUMP_PS=<hash>[,<hash>...]: those pixel shaders' microcode, each logged once, when first drawn.
    static std::vector<uint64_t> dump_ps = [] {
      std::vector<uint64_t> out;
      if (const char* v = std::getenv("NXR_DUMP_PS")) {
        for (const char* q = v; *q;) {
          char* e = nullptr;
          const uint64_t h = std::strtoull(q, &e, 16);
          if (e == q) break;
          out.push_back(h);
          q = *e == ',' ? e + 1 : e;
        }
      }
      return out;
    }();
    if (!dump_ps.empty() && pixel_shader) {
      const auto it = std::find(dump_ps.begin(), dump_ps.end(), pixel_shader->ucode_data_hash());
      if (it != dump_ps.end()) {
        REXLOG_INFO("[native_x_renderer] PS {:016X} (VS {:016X}) disassembly:\n{}", *it, vs_hash,
                    pixel_shader->ucode_disassembly());
        dump_ps.erase(it);
      }
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
  // Battlefield 1943's sky (runs sky4 to skyB, Wake Island, 14 Nov 2026), drawn after the view's opaque draws in
  // layers (what was taken for a haze over it, 0365AC9437C0240E, is the terrain's tyre tracks: D243), the sky dome
  // (8ABF56776060E64C: six draws of 7,000 to 8,000 vertices; E059F66662ECF0A4 draws the same into the half size
  // effects layer, which is laid back over the picture), a tint added over it (471C5A0705715DB7, 240 vertices) and
  // its painted clouds as sprites (E20CB2628975A77D). With a sky of the renderer's own (sky.h) none is drawn; the
  // last two only into a target of the output's size (the effects layer keeps what else they may draw there).
  if (vertex_shader->ucode_data_hash() == 0x00AB67BFF5297087ull) {  // the sky dome: its camera (DomeCamera)
    std::memcpy(dome_m_, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X], sizeof(dome_m_));
    dome_frame_ = device_->frames_presented();
  }
  {  // NXR_SKY_PHASE_LOG=1 (a diagnosis aid): each pixel shader drawn just before or after the sky dome's draws (the
     // 8 before, the 24 after), once, with its vertex shader, its vertices and whether its target is the output's size:
     // the sky's shaders on every map and their variants on frames with an effect (D179, D201).
    static const bool on = std::getenv("NXR_SKY_PHASE_LOG") != nullptr;
    if (on && pixel_shader) {
      struct Seen {
        uint64_t ps = 0, vs = 0;
        uint32_t vertices = 0;
        bool full = false;
      };
      static Seen ring[8];
      static uint32_t ring_n = 0;
      static int after = 0;
      static std::set<std::pair<uint64_t, uint64_t>> said;
      const Seen s{pixel_shader->ucode_data_hash(), vertex_shader->ucode_data_hash(), primitives.host_draw_vertex_count,
                   targets[0] && targets[0]->guest_width == jitter_size_[0] &&
                       targets[0]->guest_height == jitter_size_[1]};
      auto say = [&](const Seen& x, const char* when) {
        if (x.ps && said.insert({x.ps, x.vs}).second)
          REXLOG_INFO("[native_x_renderer] sky phase: pixel shader {:016X} vertex shader {:016X}, {} vertices, {} "
                      "target, {} the dome (frame {})",
                      x.ps, x.vs, x.vertices, x.full ? "an output sized" : "another", when, device_->frames_presented());
      };
      if (s.vs == 0x00AB67BFF5297087ull) {
        for (uint32_t i = 0; i < 8; ++i) say(ring[(ring_n + i) % 8], "before");
        after = 24;
      } else if (after > 0) {
        --after;
        say(s, "after");
      }
      ring[ring_n++ % 8] = s;
    }
  }
  // D201: the draws since the sky dome's in this frame (the sky's tint and painted clouds come right after it).
  static uint32_t draws_since_dome = ~0u;
  if (vertex_shader->ucode_data_hash() == 0x00AB67BFF5297087ull) {
    draws_since_dome = 0;
  } else if (draws_since_dome != ~0u) {
    ++draws_since_dome;
  }
  if (skip_game_sky_ && pixel_shader) {
    const uint64_t ps = pixel_shader->ucode_data_hash();
    const uint64_t vs = vertex_shader->ucode_data_hash();
    const uint32_t vertices = primitives.host_draw_vertex_count;
    const bool full_size = targets[0] && targets[0]->guest_width == jitter_size_[0] &&
                           targets[0]->guest_height == jitter_size_[1];
    // D201: the vertex shaders and vertex counts of the sky's tint and painted clouds as the known pixel shaders below
    // draw them (Guadalcanal's tint 1CE8C9384750D860: vertex shader 65BD09519504B2B5, 240 vertices; run gfire1).
    static std::set<std::pair<uint64_t, uint32_t>> sky_shapes;
    static std::set<uint64_t> said_variant;
    auto note = [&](const char* rule) {  // D249
      NoteRuleSkip(rule, ps, vs, vertices, targets[0] ? targets[0]->guest_width : 0,
                   targets[0] ? targets[0]->guest_height : 0, draws_since_dome);
    };
    auto sky_shape = [&](const char* rule) {
      note(rule);
      if (full_size && sky_shapes.size() < 64) sky_shapes.insert({vs, vertices});
      return false;
    };
    // D243: not 0365AC9437C0240E nor 38DEA3BE39271907, taken for the sky's haze (drawn before the dome, 180 to 1,500
    // vertices, blended): by the game's own constant tables the first is the terrain's tyre tracks (the outdoor light,
    // the sun's shadow map, the heightmap, six textures with the tread, the dynamic mask's tile and indirection maps),
    // the second samples three textures as Wake Island's track strips do (617653D17C0D6DD5). Skipped, every map's tyre
    // tracks were missing with the renderer's own sky on (the user, 24 Nov 2026: "all the tire tracks are perfect in
    // the SDK renderer"; RenderDoc at flag A: the SDK's eight blended terrain draws over the sand, ours none).
    if (ps == 0x8ABF56776060E64Cull || ps == 0xE059F66662ECF0A4ull) {
      note("the game's sky dome (D179)");
      return false;
    }
    // 10FA030CD9308DE2: the tint's shader in frames with an effect drawn (a muzzle flash, an explosion: the game then
    // draws this part of its sky with other shaders, runs skyfx_n8): left drawn, the game's own sky and painted clouds
    // came back over the renderer's for as long as the effect ("when shooting and explosions trigger, they hide our sky
    // and show the original sky", the user, 16 Nov 2026).
    // D249: E20CB2628975A77D, the painted clouds' sprites, is also the effects' particle shader (D195), so it was checked:
    // at the output's size its draws come from 2 to 64 draws after the dome, all with one texture, each map's sky sheet
    // (format 59, 2048 x 1024; runs skipe_m0 to m3, NXR_E20C_LOG), and drawn they change nothing seen (the effects draw
    // into the half size effects layer, which this leaves). Passed over wherever they come; NXR_SKY_SPRITES_NEAR=<n> (a
    // diagnosis aid) passes over only those within n draws after the dome.
    static const uint32_t sprites_near = [] {
      const char* v = std::getenv("NXR_SKY_SPRITES_NEAR");
      return v && *v ? uint32_t(std::atoi(v)) : ~0u;
    }();
    // NXR_E20C_LOG=1 (a diagnosis aid, D249): each distinct first texture E20CB2628975A77D draws with at the output's
    // size, with how many draws after the dome: the clouds' sprites and the effects' textures told apart.
    static const bool e20c_log = std::getenv("NXR_E20C_LOG") != nullptr;
    if (e20c_log && ps == 0xE20CB2628975A77Dull && full_size) {
      if (const uint32_t mask = pixel_shader->GetUsedTextureMaskAfterTranslation()) {
        const auto f = regs.GetTextureFetch(uint32_t(std::countr_zero(mask)));
        const uint32_t since = std::min<uint32_t>(draws_since_dome, 999);
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> said;
        if (said.size() < 400 && said.insert({uint32_t(f.format), uint32_t(f.size_2d.width) + 1,
                                              uint32_t(f.size_2d.height) + 1, uint32_t(f.base_address), since})
                                     .second)
          REXLOG_INFO("[native_x_renderer] E20C: texture format {} {}x{} at {:08X}, {} draws after the dome, {} "
                      "vertices",
                      uint32_t(f.format), uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1,
                      uint32_t(f.base_address), since, vertices);
      }
    }
    if ((ps == 0x471C5A0705715DB7ull || ps == 0x10FA030CD9308DE2ull ||
         (ps == 0xE20CB2628975A77Dull && draws_since_dome <= sprites_near)) &&
        full_size) {
      return sky_shape("Wake Island's sky tint and painted clouds (D179, D201)");
    }
    // Guadalcanal's, Iwo Jima's and Coral Sea's sky (runs map2_night, map3_night, 17 Nov 2026: the game's painted
    // sunset and clouds over the renderer's night): the same dome, but its tint by 1106AB346E573399 (240 vertices)
    // and its painted clouds by 3206097F1E8CC042 (vertex shader 63302D0C99305DF8, batches of 396 and 576 vertices).
    // Guadalcanal's tint and painted clouds: EE301688055DFECB and 1CE8C9384750D860 (run map1_skip: with them and its
    // haze skipped, the renderer's night sky and clouds; drawn, the game's daylit blue and clouds at 21 h).
    // 062964B870D68445: Guadalcanal's tint in frames with an effect, as 10FA030CD9308DE2 is Wake's (run gfire1: the
    // game's daylit blue sky and painted clouds round the renderer's night sky while firing at 21 h; the user, 19 Nov
    // 2026: "The bug with gunshots and explosions that I thought we fixed is happening on Guadalcanal").
    if ((ps == 0x1106AB346E573399ull || ps == 0xEE301688055DFECBull || ps == 0x1CE8C9384750D860ull ||
         ps == 0x062964B870D68445ull ||
         (ps == 0x3206097F1E8CC042ull && vs == 0x63302D0C99305DF8ull)) &&
        full_size) {
      return sky_shape("the other maps' sky tint and painted clouds (D179, D201)");
    }
    // D201: any other pixel shader drawing a sky shape (a known tint's or painted clouds' vertex shader and vertex
    // count) into the output sized target within the 24 draws after the dome is the same part of the sky drawn by a
    // variant (each map's, on frames with an effect): passed over too, so maps whose variants were never seen keep
    // the renderer's sky while firing.
    if (full_size && draws_since_dome <= 24 && sky_shapes.count({vs, vertices})) {
      if (said_variant.size() < 32 && said_variant.insert(ps).second)
        REXLOG_INFO("[native_x_renderer] sky: pixel shader {:016X} draws the sky's shape (vertex shader {:016X}, {} "
                    "vertices) after the dome: passed over as the sky's (D201)",
                    ps, vs, vertices);
      note("a sky shape after the dome (D201)");
      return false;
    }
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
  std::chrono::steady_clock::time_point wait_start{};
  bool waited = false;
  auto may_wait = [&] {
    if (wait_start == std::chrono::steady_clock::time_point{}) wait_start = std::chrono::steady_clock::now();
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
  ITextureView* depth_view = nullptr;
  uint32_t render_target_count = 0, samples = 0;
  PipelineState state;
  state.Reset();
  HostTarget* depth = (depth_control.z_enable || depth_control.stencil_enable) ? targets[4] : nullptr;
  // Host MSAA (targets.h): into the targets' multisampled twins when every target the draw binds has one, of one
  // sample count; else into their one-sample textures.
  uint32_t msaa = 0;
  bool msaa_alike = true;
  auto twin = [&](const HostTarget* t) {
    const uint32_t n = t->msaa_view ? t->msaa_samples : 1;
    if (msaa && msaa != n) msaa_alike = false;
    msaa = n;
  };
  for (uint32_t i = 0; i < 4; ++i)
    if (((color_mask >> (4 * i)) & 0xF) && targets[i]) twin(targets[i]);
  if (depth) twin(depth);
  const bool multisampled = msaa_alike && msaa > 1;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) continue;
    HostTarget* target = targets[i];
    if (!target) {
      color_mask &= ~(0xFu << (4 * i));
      continue;
    }
    render_target_views[i] = HostMsaaDrawView(target, multisampled, true);
    state.rtv_formats[i] = uint16_t(target->format);
    render_target_count = i + 1;
    samples = multisampled ? msaa : target->samples;
  }
  if (!render_target_count && !depth) {
    ++stats_.no_targets;
    return false;
  }
  if (depth) {
    const uint32_t depth_samples = multisampled ? msaa : depth->samples;
    if (samples && depth_samples != samples) {
      NATIVE_X_RENDERER_WARN_ONCE("[native_x_renderer] color and depth targets with different sample counts");
      ++stats_.failed;
      return false;
    }
    samples = depth_samples;
    state.dsv_format = uint16_t(depth->format);
    depth_view = HostMsaaDrawView(depth, multisampled,
                                  (depth_control.z_enable && depth_control.z_write_enable) || depth_control.stencil_enable);
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
  // How much of the game is drawn this way (13 Nov 2026, the rendering quirks: a dither that shimmers, or none at
  // all?): its draws with alpha to coverage counted, said once after the first 2000 and how they are drawn.
  if (pixel_shader && regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable) {
    static uint64_t seen = 0, blended = 0, at_1x = 0;
    ++seen, blended += alpha_to_coverage_blended, at_1x += !guest_msaa;
    if (seen == 2000)
      REXLOG_INFO("[native_x_renderer] alpha to coverage: the game's first 2000 such draws, {} on surfaces it draws at "
                  "1x, {} blended by alpha and {} dithered (native_x_renderer_alpha_to_coverage)",
                  at_1x, blended, seen - blended);
  }
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
            memory_->EnsureMirror(write.address, uint32_t(std::min<uint64_t>(uint64_t((write.pitch + 31) & ~31u) * ((write.height + 31) & ~31u) * 16, 0x20000000u)));  // D174
            if (blit_->WriteToMirror(device_->context(), t->ArrayView(), memory_->buffer(), memory_->uav(), write)) {
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
    if (memory_->WriteIfChanged(copy.address, copy.data, copy.size)) ++stats_.inline_restored;
  }
  if (primitives_->UploadConverted()) ++stats_.copies;
  // This draw's own data or textures copied, ending the render pass: the queued draws' data copied now too (D98).
  if (upload_ahead_ && memory_->copies() != copies_at_start) upload_ahead_();
  stats_.copies += memory_->TakeCopies();
  stats_.uploaded_bytes += memory_->TakeUploadedBytes();
  stats_.unchanged_bytes += memory_->TakeUnchangedBytes();
  stats_.unwatched_hashed_bytes += memory_->TakeUnwatchedHashedBytes();
  memory_->PrepareForRead();

  // Constants.
  DrawContext draw{&regs, &primitives, &viewport_info, polygonal, used_texture_mask, alpha_to_coverage_blended,
                   state.depth_enable && state.depth_write};
  FillSystemConstants(draw);
  uploads_.count = 0;
  {
    MappedBuffer map(recorder, system_constants_buffer_, &uploads_);
    std::memcpy(map.data(), &system_constants_, sizeof(system_constants_));
  }
  const rg::SpirvShader* stage_shaders[2] = {vertex_shader, pixel_shader};
  // The vertex shader's are also kept for the object motion (D48).
  vertex_constants_.clear();
  vertex_registers_.clear();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rg::SpirvShader* shader = stage_shaders[stage];
    if (!shader || !shader->constant_register_map().float_count) continue;
    MappedBuffer map(recorder, pipeline->float_constants[stage], &uploads_);
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
          size_t registers = vertex_registers_.size();
          vertex_registers_.resize(registers + count);
          std::iota(vertex_registers_.begin() + registers, vertex_registers_.end(), uint8_t(first_register));
        }
        out += bytes;
      }
    }
    // D154: a model placed from another camera than the frame's, moved onto it (SetAttachedCameraMove).
    if (!stage) MoveAttached(reinterpret_cast<const uint32_t*>(&regs[0]), first, constants.float_bitmap, mapped_start);
    if (!stage) MoveWeaponLayer(reinterpret_cast<const uint32_t*>(&regs[0]), first, constants.float_bitmap, mapped_start);
    if (!stage) {
      ClassifyWeapon(reinterpret_cast<const uint32_t*>(&regs[0]), first, constants.float_bitmap, viewport_info.z_min,
                     viewport_info.z_max);
      // D217: a vehicle's body in the rain (the user, 19 Nov 2026: "I though we applied weapon the wetness shader to
      // vehicle externals too", then "just vehicles"): known by its draw's index count (the night lamps' models: the
      // jeeps' and the tanks' bodies, the landing boat's), drawn with the weapon's wet copies of its colour texture.
      vehicle_draw_ = false;
      if (wet_models_ && !weapon_draw_ && wet_count_ * wet_show_ >= 0.01f) {
        const uint32_t n = primitives.guest_draw_vertex_count;
        for (size_t k = 0; k + 1 < wet_models_->size(); k += 2)
          if ((*wet_models_)[k] == n) vehicle_draw_ = true;
      }
    }
    ApplyOutdoorLight(shader->ucode_data_hash(), stage, reinterpret_cast<const uint32_t*>(&regs[0]), first,
                      constants.float_bitmap, mapped_start);
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
    MappedBuffer map(recorder, bool_loop_constants_buffer_, &uploads_);
    std::memcpy(map.data(), bool_loop, kBoolLoopConstantsSize);
    std::memcpy(uploaded_bool_loop_.data(), bool_loop, kBoolLoopConstantsSize);
    uploaded_frame_ = frame;
  }
  {
    MappedBuffer map(recorder, fetch_constants_buffer_, &uploads_);
    std::memcpy(map.data(), &regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    // 13 Nov 2026 ("set a clamp on texture bias with Anisotropic Filtering enabled"): with anisotropic filtering
    // forced (anisotropic_override 2x and up), a texture's own negative LOD bias (its fetch constant's word 4, bits
    // 12 to 21, in 1/32 of a level; the translated shaders scale their gradients by it) is taken as none. A game sets
    // it to sharpen a trilinear picture at 720 lines; anisotropic filtering does that without the sparkle a sharper
    // level than the screen can show gives in motion. Only the constants this draw's shaders sample as textures: the
    // same registers hold vertex fetches.
    if (const int32_t forced = REXCVAR_GET(anisotropic_override); forced >= 2 && forced < 6) {
      auto* words = reinterpret_cast<uint32_t*>(map.data());
      for (uint32_t mask = used_texture_mask; mask; mask &= mask - 1) {
        uint32_t& word = words[std::countr_zero(mask) * 6 + 4];
        if (word & (1u << 21)) word &= ~(0x3FFu << 12);  // the field's sign bit: a negative bias
      }
    }
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
          NoteShadowMap(stage, binding.fetch_constant, view);
        if (stage == 1 && weapon_draw_ && view && WeaponExportOn()) export_textures_.push_back({binding.fetch_constant, view});
        if (stage == 1 && !weapon_draw_ && skin_gun_address_ &&
            uint32_t(regs.GetTextureFetch(binding.fetch_constant).base_address) == skin_gun_address_)
          skin_3p_hit_ = true;
        view = SkinView(stage, binding.fetch_constant, view, *shader, regs);
        view = WetView(stage, binding.fetch_constant, view, *shader, regs);
        view = SeaSkyView(stage, binding.fetch_constant, view, *shader,
                          binding.dimension == rg::xenos::FetchOpDimension::kCube);
        {  // NXR_WEAPON_LOG=2 (research): 95 s in, each pixel shader's textures with the draw's depth range, once
          static const bool log2 = [] {
            const char* v = std::getenv("NXR_WEAPON_LOG");
            return v && *v == '2';
          }();
          if (log2 && stage == 1 && view) {
            static const auto since = std::chrono::steady_clock::now();
            static std::set<uint64_t> said2;
            const auto f = regs.GetTextureFetch(binding.fetch_constant);
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count() > 95.0 &&
                f.size_2d.width + 1 >= 256 && said2.size() < 400 &&
                said2.insert(shader->ucode_data_hash() * 131 + binding.fetch_constant * 7 + uint64_t(viewport_info.z_max * 1000)).second) {
              REXLOG_INFO("[native_x_renderer] tex: PS {:016X} z {:.4f} {:.4f} fetch {} format {} {}x{} base {:05X} cam0 {}",
                          shader->ucode_data_hash(), viewport_info.z_min, viewport_info.z_max, binding.fetch_constant,
                          uint32_t(f.format), f.size_2d.width + 1, f.size_2d.height + 1, uint32_t(f.base_address),
                          weapon_draw_ ? 1 : 0);
            }
          }
        }
        // NXR_WEAPON_LOG=1 (research for the wetness): the first person weapon's and hands' textures, each once.
        if (stage == 1 && weapon_draw_ && view) {
          static const bool log = [] {
            const char* v = std::getenv("NXR_WEAPON_LOG");
            return v && *v == '1';
          }();
          static std::set<uint64_t> said;
          const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
          const uint64_t key = shader->ucode_data_hash() * 31 + binding.fetch_constant + (uint64_t(fetch.base_address) << 8);
          if (log && said.size() < 120 && said.insert(key).second) {
            const TextureDesc& d = static_cast<ITextureView*>(view)->GetTexture()->GetDesc();
            REXLOG_INFO("[native_x_renderer] weapon texture: PS {:016X} fetch {} guest format {} {}x{} base {:05X}; host "
                        "{}x{} format {} mips {} name {}", shader->ucode_data_hash(), binding.fetch_constant,
                        uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                        uint32_t(fetch.base_address), d.Width, d.Height, int(d.Format), d.MipLevels, d.Name ? d.Name : "");
          }
        }
        }
        binding_objects_.push_back(view);
        continue;
      }
      if (IShaderResourceVariable* variable = pipeline->textures[stage][i]) {
        const auto& binding = texture_bindings[i];
        IDeviceObject* view = textures_->View(binding.fetch_constant, regs.GetTextureFetch(binding.fetch_constant),
                                              binding.dimension, binding.is_signed);
        NoteShadowMap(stage, binding.fetch_constant, view);
        if (stage == 1 && weapon_draw_ && view && WeaponExportOn()) export_textures_.push_back({binding.fetch_constant, view});
        if (stage == 1 && !weapon_draw_ && skin_gun_address_ &&
            uint32_t(regs.GetTextureFetch(binding.fetch_constant).base_address) == skin_gun_address_)
          skin_3p_hit_ = true;
        view = SkinView(stage, binding.fetch_constant, view, *shader, regs);
        view = WetView(stage, binding.fetch_constant, view, *shader, regs);
        view = SeaSkyView(stage, binding.fetch_constant, view, *shader,
                          binding.dimension == rg::xenos::FetchOpDimension::kCube);
        {  // NXR_WEAPON_LOG=2 (research): 95 s in, each pixel shader's textures with the draw's depth range, once
          static const bool log2 = [] {
            const char* v = std::getenv("NXR_WEAPON_LOG");
            return v && *v == '2';
          }();
          if (log2 && stage == 1 && view) {
            static const auto since = std::chrono::steady_clock::now();
            static std::set<uint64_t> said2;
            const auto f = regs.GetTextureFetch(binding.fetch_constant);
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count() > 95.0 &&
                f.size_2d.width + 1 >= 256 && said2.size() < 400 &&
                said2.insert(shader->ucode_data_hash() * 131 + binding.fetch_constant * 7 + uint64_t(viewport_info.z_max * 1000)).second) {
              REXLOG_INFO("[native_x_renderer] tex: PS {:016X} z {:.4f} {:.4f} fetch {} format {} {}x{} base {:05X} cam0 {}",
                          shader->ucode_data_hash(), viewport_info.z_min, viewport_info.z_max, binding.fetch_constant,
                          uint32_t(f.format), f.size_2d.width + 1, f.size_2d.height + 1, uint32_t(f.base_address),
                          weapon_draw_ ? 1 : 0);
            }
          }
        }
        // NXR_WEAPON_LOG=1 (research for the wetness): the first person weapon's and hands' textures, each once.
        if (stage == 1 && weapon_draw_ && view) {
          static const bool log = [] {
            const char* v = std::getenv("NXR_WEAPON_LOG");
            return v && *v == '1';
          }();
          static std::set<uint64_t> said;
          const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
          const uint64_t key = shader->ucode_data_hash() * 31 + binding.fetch_constant + (uint64_t(fetch.base_address) << 8);
          if (log && said.size() < 120 && said.insert(key).second) {
            const TextureDesc& d = static_cast<ITextureView*>(view)->GetTexture()->GetDesc();
            REXLOG_INFO("[native_x_renderer] weapon texture: PS {:016X} fetch {} guest format {} {}x{} base {:05X}; host "
                        "{}x{} format {} mips {} name {}", shader->ucode_data_hash(), binding.fetch_constant,
                        uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                        uint32_t(fetch.base_address), d.Width, d.Height, int(d.Format), d.MipLevels, d.Name ? d.Name : "");
          }
        }
        if (pipeline->texture_objects[stage][i] != view) {
          recorder.Do([variable, view](IDeviceContext*) { variable->Set(view); });
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
          recorder.Do([variable, sampler](IDeviceContext*) { variable->Set(sampler); });
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
      uploaded_frame_ = UINT64_MAX;
      return false;
    }
    if (created) ++stats_.binding_sets_created;
  }

  // D190: the picture just after the sea's last draw, taken before the next draw that is not the sea's.
  if (sea_after_due_ && !(pixel_shader && IsSeaShader(pixel_shader->ucode_data_hash()))) TakeSeaAfter();
  // Record the draw. The targets are bound only when they change (D30).
  std::array<ITextureView*, 5> views{render_target_views[0], render_target_views[1], render_target_views[2],
                                     render_target_views[3], depth_view};
  if (!bound_valid_ || bound_count_ != render_target_count || bound_ != views) {
    recorder.Do([views, count = render_target_count](IDeviceContext* context) mutable {
      context->SetRenderTargets(count, views.data(), views[4], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    });
    bound_ = views;
    bound_count_ = render_target_count;
    bound_valid_ = true;
    viewport_valid_ = false;  // DiligentCore sets the viewport to the targets' size (D81)
    committed_ = nullptr;     // and the shader resources are committed again, transitioning what was a target (D82)
  }
  recorder.Do([pso = pipeline->pso.RawPtr(), uploads = uploads_](IDeviceContext* context) {
    for (uint32_t i = 0; i < uploads.count; ++i) Upload(context, uploads.list[i].buffer, uploads.list[i].size, uploads.list[i].data);
    context->SetPipelineState(pso);
  });
  // The shader resources are committed when something changed since the last draw committed them (D82): another
  // pipeline, a texture or sampler, the render targets, or a texture no longer readable (loaded, copied or drawn
  // into since). DiligentCore allocates and writes a descriptor set for the textures and samplers on every
  // commit; in Kameo's first level three draws in four have the last draw's pipeline and resources.
  if (set) {
    // D111: the set's binding, its variables written when it was created; committed when it is not the last
    // committed one, or one of its textures is no longer readable.
    const bool changed = committed_ != set;
    recorder.Do([set, changed](IDeviceContext* context) {
      if (changed || !TexturesReadable(*set))
        context->CommitShaderResources(set->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    });
    committed_ = set;
    if (changed) ++stats_.committed;
  } else if (IDeviceContext* context = device_->context();
             bindings_changed || committed_ != pipeline || !TexturesReadable(*pipeline)) {
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
  // NXR_XHAIR_LOG=1 (a developer's, 24 Nov 2026: which of the crosshair sheet's draws is the hit marker, sprite 5 at
  // 180, 2, 32 x 32 on the sheet): each kind of HUD draw from the sheet, once: its vertex layout and first vertices.
  static const bool xhair_log = [] {
    const char* v = std::getenv("NXR_XHAIR_LOG");
    return v && *v == '1';
  }();
  if (xhair_log && !state.depth_enable && pixel_shader) {
    if (const uint32_t mask = pixel_shader->GetUsedTextureMaskAfterTranslation()) {
      uint32_t first = 0;
      while (!(mask & (1u << first))) ++first;
      const auto fetch = regs.GetTextureFetch(first);
      auto* kernel = rex::system::kernel_state();
      const auto& bindings = vertex_shader->vertex_bindings();
      if (uint32_t(fetch.format) == 6 && fetch.size_2d.width + 1 == 256 && fetch.size_2d.height + 1 == 256 && kernel &&
          kernel->memory() && !bindings.empty()) {
        const auto& binding = bindings[0];
        const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(binding.fetch_constant);
        const uint8_t* data = kernel->memory()->TranslatePhysical<const uint8_t*>(vf.address << 2);
        const uint32_t stride = binding.stride_words * 4, count = registers_->values[rg::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16;
        std::string layout, verts;
        for (const auto& attribute : binding.attributes) {
          const auto& a = attribute.fetch_instr.attributes;
          layout += fmt::format(" {}@{}", int(a.data_format), a.offset * 4);
        }
        for (uint32_t v = 0; v < std::min<uint32_t>(count, 6) && data && stride; ++v) {
          verts += " [";
          for (uint32_t w = 0; w < stride / 4 && w < 8; ++w) {
            uint32_t word;
            std::memcpy(&word, data + size_t(v) * stride + w * 4, 4);
            word = rex::byte_swap(word);
            float f;
            std::memcpy(&f, &word, 4);
            const uint16_t hi = uint16_t(word >> 16), lo = uint16_t(word);
            verts += fmt::format(" {:.4g}|{:04X}.{:04X}", f, hi, lo);
          }
          verts += " ]";
        }
        // The vertex shader's float constants it uses (the sprite's place on the sheet is a texture matrix there).
        {
          const auto& cm = vertex_shader->constant_register_map();
          int shown = 0;
          verts += " consts";
          for (uint32_t r = 0; r < 256 && shown < 16; ++r) {
            if (!(cm.float_bitmap[r >> 6] & (uint64_t(1) << (r & 63)))) continue;
            float c[4];
            std::memcpy(c, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + r * 4], sizeof(c));
            verts += fmt::format(" c{}=({:.4g} {:.4g} {:.4g} {:.4g})", r, c[0], c[1], c[2], c[3]);
            ++shown;
          }
        }
        static std::set<std::string> said;
        const std::string key = fmt::format("{:016X}{:016X}{}{}", vertex_program.hash, pixel_program.hash, count, layout);
        if (said.size() < 64 && said.insert(key + verts.substr(0, 60)).second)
          REXLOG_INFO("[native_x_renderer] crosshair sheet draw: vs {:016X} ps {:016X} {} vertices, stride {}, layout{}:{}",
                      vertex_program.hash, pixel_program.hash, count, stride, layout, verts);
      }
    }
  }
  // HideCrosshair: a HUD draw (no depth test) whose first texture is the crosshair sheet is left out, but for the hit
  // marker (the user, 24 Nov 2026: "we also hide the hit marker. We need to keep the hit marker shown"). The sheet's
  // sprites are quads in the movie's units with no texture coordinates; the vertex shader's c11 and c12 are the
  // texture matrix (u = c11.x x + c11.w, v = c12.y y + c12.w; run xh2): a quad that lands within the hit marker's
  // place on the sheet (sprite 5: 180, 2, 32 x 32 pixels; its quad 36 across) is drawn.
  if (!state.depth_enable && pixel_shader &&
      g_hide_crosshair_until.load(std::memory_order_relaxed) > std::chrono::steady_clock::now().time_since_epoch().count()) {
    if (const uint32_t mask = pixel_shader->GetUsedTextureMaskAfterTranslation()) {
      uint32_t first = 0;
      while (!(mask & (1u << first))) ++first;
      const auto fetch = regs.GetTextureFetch(first);
      if (uint32_t(fetch.format) == 6 && fetch.size_2d.width + 1 == 256 && fetch.size_2d.height + 1 == 256) {
        bool hit_marker = false;
        auto* kernel = rex::system::kernel_state();
        const auto& bindings = vertex_shader->vertex_bindings();
        const uint32_t count = registers_->values[rg::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16;
        if (kernel && kernel->memory() && !bindings.empty() && count && count <= 64 && bindings[0].stride_words == 2) {
          const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(bindings[0].fetch_constant);
          const uint8_t* data = kernel->memory()->TranslatePhysical<const uint8_t*>(vf.address << 2);
          float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
          for (uint32_t v = 0; data && v < count; ++v)
            for (int i = 0; i < 2; ++i) {
              uint32_t word;
              std::memcpy(&word, data + size_t(v) * 8 + i * 4, 4);
              word = rex::byte_swap(word);
              float f;
              std::memcpy(&f, &word, 4);
              if (std::isfinite(f)) lo[i] = std::min(lo[i], f), hi[i] = std::max(hi[i], f);
            }
          float c11[4], c12[4];
          std::memcpy(c11, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + 11 * 4], sizeof(c11));
          std::memcpy(c12, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + 12 * 4], sizeof(c12));
          if (lo[0] <= hi[0] && lo[1] <= hi[1]) {
            // The quad's corners on the sheet, in its pixels.
            const float u0 = (c11[0] * lo[0] + c11[3]) * 256.0f, u1 = (c11[0] * hi[0] + c11[3]) * 256.0f;
            const float v0 = (c12[1] * lo[1] + c12[3]) * 256.0f, v1 = (c12[1] * hi[1] + c12[3]) * 256.0f;
            const float ul = std::min(u0, u1), ur = std::max(u0, u1), vt = std::min(v0, v1), vb = std::max(v0, v1);
            hit_marker = ul > 170.0f && ur < 222.0f && vt > -6.0f && vb < 42.0f && ur - ul > 8.0f;
          }
        }
        if (!hit_marker) {
          NoteRuleSkip("the crosshair while aiming (HideCrosshair)", pixel_shader->ucode_data_hash(),
                       vertex_shader->ucode_data_hash(), count, 0, 0, ~0u);  // D249
          return false;
        }
        static bool said = false;
        if (!said) {
          said = true;
          REXLOG_INFO("[native_x_renderer] crosshair hidden while aiming: the hit marker drawn");
        }
      }
    }
  }
  HudPlace place;
  place.align = aspect_align_;
  const bool hud_scaling = hud_scale_ != 1.0f || minimap_scale_ != 1.0f;
  bool hud_placed = false;
  uint32_t hud_pixel_mask = 0;
  if (hud_classify_ && ((aspect_box_ && target_scale.x != target_scale.y) || hud_scaling)) {
    hud_sheet_w_ = hud_sheet_h_ = hud_sheet_format_ = 0;
    if (const uint32_t pixel_mask = pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0) {
      hud_pixel_mask = pixel_mask;
      uint32_t first = 0;
      while (!(pixel_mask & (1u << first))) ++first;
      const auto fetch = regs.GetTextureFetch(first);
      hud_sheet_w_ = fetch.size_2d.width + 1, hud_sheet_h_ = fetch.size_2d.height + 1;
      hud_sheet_format_ = uint32_t(fetch.format);
    }
    place = ClassifyHudDraw(copies, vertex_program.hash), hud_placed = true;
  }
  // NXR_BOX_LOG=1 (a diagnosis aid, 19 Nov 2026: the loading screen's fade to black boxed at 16:9 on a wider screen):
  // each boxed draw whose viewport covers the whole canvas, with how it was placed.
  if (aspect_box_ && place.mode != HudMode::kStretch && target_scale.x != target_scale.y) {
    static const bool box_log = std::getenv("NXR_BOX_LOG") != nullptr;
    static std::set<uint64_t> box_said;  // once per shader, mode and extent (the same fade every frame filled the log)
    const uint64_t key = vertex_program.hash ^ pixel_program.hash ^ uint64_t(place.mode) << 56 ^
                         uint64_t(std::lround(place.lo * 20) & 0xFF) << 48 ^ uint64_t(std::lround(place.hi * 20) & 0xFF) << 40;
    if (box_log && box_said.size() < 400 && viewport_info.xy_extent[0] >= sized->guest_width &&
        viewport_info.xy_extent[1] >= sized->guest_height && box_said.insert(key).second) {
      REXLOG_INFO("[native_x_renderer] box log: vs {:016X} ps {:016X} textures {:X} mode {} placed {} classify {} menu {} "
                  "clip x {:.2f}..{:.2f} y {:.2f}..{:.2f}, {} vertices, {} copies, {} registers",
                  vertex_program.hash, pixel_program.hash, hud_pixel_mask, int(place.mode), hud_placed, hud_classify_,
                  hud_menu_, place.lo, place.hi, place.y_lo, place.y_hi,
                  registers_->values[rg::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16, copies.size(), vertex_registers_.size());
    }
  }
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
  // The HUD's scale (draw.h SetHudScale): about the screen's point the draw's part of the HUD hangs on. Not a world
  // marker (13 Nov 2026: "do not change 3D Flag icons scaling": the flags' icons over the map keep the game's size),
  // not a menu's frames, nor what is stretched or reaches to an edge.
  if (hud_placed && hud_scaling && !hud_menu_ && place.mode == HudMode::kBox) {
    const float gw = float(sized->guest_width), gh = float(sized->guest_height);
    // With the HUD at the edges, a draw's side is its box's (align); with a centred HUD (every 16:9 screen) the box is
    // in the middle, so its side is where it sits on the canvas (anchor_x): there the minimap slider found no minimap
    // (the review of pull request 16).
    const float side = hud_edges_ ? place.align : place.anchor_x;
    const bool minimap = place.mode == HudMode::kBox && side == 0.0f && place.anchor_y == 1.0f;
    const float k = minimap ? minimap_scale_ : hud_scale_;
    if (k != 1.0f && k > 0.1f) {
      const float ax = side * gw, ay = place.anchor_y * gh;
      offset_x += ax * scale_x * (1.0f - k);
      offset_y += ay * scale_y * (1.0f - k);
      scale_x *= k, scale_y *= k;
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
  // D168: the sniper reticle cut to the picture in picture lens's square while the lens is drawn (SetScopeClip): its
  // pieces are the HUD sprite shaders' small quads stretched by their constants into lines across the screen (run
  // v41scope20: the 104th 2D draw, a 50 x 33 quad, drew the level line through the lens), so a piece is told by its
  // size in clip space: a quarter of the screen or more one way (2 is the screen's width). The HUD's pictures drawn
  // with them (flags, icons, ammo) are far smaller.
  if (scope_clip_.right > scope_clip_.left && output_sized && vertex_program.hash == 0xBCFA50F3EA22BAE7ull &&
      pixel_program.hash == 0x90AC5F14E9463FB2ull) {
    double e[4];
    const bool placed = HudClipExtent(copies, regs[rg::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16, e);
    const bool reticle = placed && std::max(e[1] - e[0], e[3] - e[2]) >= 0.5;
    if (scope_log_ && scope_logged_ <= 400)
      REXLOG_INFO("[native_x_renderer] scope research: a sprite quad{}{}",
                  placed ? fmt::format(" at clip x {:.2f}..{:.2f} y {:.2f}..{:.2f}", e[0], e[1], e[2], e[3])
                         : std::string(" not placed"),
                  reticle ? ": the reticle, cut to the lens" : "");
    if (reticle) {
      for (Rect* r : {&scissor_rect, &stretched_scissor}) {
        r->left = std::max(r->left, scope_clip_.left), r->top = std::max(r->top, scope_clip_.top);
        r->right = std::max(r->left, std::min(r->right, scope_clip_.right));
        r->bottom = std::max(r->top, std::min(r->bottom, scope_clip_.bottom));
      }
    }
  }
  // Set when they change (D81): DiligentCore records them on every call.
  if (!viewport_valid_ || std::memcmp(&viewport, &viewport_, sizeof(viewport))) {
    recorder.Do([viewport](IDeviceContext* context) { context->SetViewports(1, &viewport, 0, 0); });
    viewport_ = viewport;
  }
  if (!viewport_valid_ || scissor_rect != scissor_) {
    recorder.Do([scissor_rect](IDeviceContext* context) { context->SetScissorRects(1, &scissor_rect, 0, 0); });
    scissor_ = scissor_rect;
  }
  viewport_valid_ = true;
  const float blend_factors[4] = {regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_GREEN),
                                  regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_ALPHA)};
  // D169 research (NXR_SKY_PROBE=1, tests): each new shader pair drawn at the output's size, once, with its vertex
  // constants c12 to c15 (where the terrain's world-view-projection is), to find the sky dome's camera.
  if (output_sized && pixel_shader && render_target_count) {
    static const bool probe = std::getenv("NXR_SKY_PROBE") != nullptr;
    static std::set<uint64_t> seen;
    static const bool probe2 = probe && std::string(std::getenv("NXR_SKY_PROBE")) == "2";
    if (probe2 && hud_frame_ % 300 == 0 && hud_frame_ > 600 && !vertex_registers_.empty()) {
      static uint64_t last_frame = 0;
      static std::set<uint64_t> in_frame;
      if (last_frame != hud_frame_) last_frame = hud_frame_, in_frame.clear();
      if (in_frame.insert(vertex_shader->ucode_data_hash()).second) {
        uint32_t matrix = 0;
        bool transposed = false;
        const bool found = CameraVote::FindMatrix(vertex_constants_.data(), vertex_registers_.data(),
                                                  uint32_t(vertex_registers_.size()), matrix, transposed);
        Mat4d mm{};
        if (found) mm = CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed);
        const double* m = found ? mm.data() : nullptr;
        if (m)
          REXLOG_INFO("[native_x_renderer] sky probe2: frame {} VS {:016X} depth {}{} matrix c{}{} rows {:.3f} {:.3f} {:.3f} {:.1f} | "
                      "{:.3f} {:.3f} {:.3f} {:.1f} | {:.3f} {:.3f} {:.3f} {:.1f} | {:.3f} {:.3f} {:.3f} {:.1f}",
                      hud_frame_, vertex_shader->ucode_data_hash(), state.depth_enable ? "test" : "off",
                      state.depth_write ? "+write" : "", matrix, transposed ? " (transposed)" : "", m[0], m[1], m[2],
                      m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        else
          REXLOG_INFO("[native_x_renderer] sky probe2: frame {} VS {:016X} depth {}{} no matrix", hud_frame_,
                      vertex_shader->ucode_data_hash(), state.depth_enable ? "test" : "off", state.depth_write ? "+write" : "");
      }
    }
    if (probe && !probe2 && seen.size() < 400) {
      const uint64_t key = vertex_shader->ucode_data_hash() * 31 + pixel_shader->ucode_data_hash();
      if (seen.insert(key).second) {
        const float* c = reinterpret_cast<const float*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X]);
        REXLOG_INFO("[native_x_renderer] sky probe: VS {:016X} PS {:016X} depth {}{} frame {} c12 {:.3f} {:.3f} {:.3f} {:.3f} | "
                    "c13 {:.3f} {:.3f} {:.3f} {:.3f} | c14 {:.4f} {:.4f} {:.4f} {:.4f} | c15 {:.3f} {:.3f} {:.3f} {:.3f}",
                    vertex_shader->ucode_data_hash(), pixel_shader->ucode_data_hash(), state.depth_enable ? "test" : "off",
                    state.depth_write ? "+write" : "", hud_frame_, c[48], c[49], c[50], c[51], c[52], c[53], c[54], c[55],
                    c[56], c[57], c[58], c[59], c[60], c[61], c[62], c[63]);
      }
    }
  }
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
  // D229 (NXR_DEVICE_FAULT=1, a diagnosis aid): an indexed draw whose indices run past the end of their buffer (index
  // reads are not bounds checked like a shader's), logged (the first 20).
  {
    static const bool check = [] {
      const char* v = std::getenv("NXR_DEVICE_FAULT");
      return v && *v == '1';
    }();
    static int said = 0;
    if (check && index_buffer && said < 20) {
      const uint64_t size = index_buffer->GetDesc().Size;
      const uint64_t end = index_offset + uint64_t(primitives.host_draw_vertex_count) * (index_type == VT_UINT16 ? 2 : 4);
      if (end > size) {
        ++said;
        REXLOG_ERROR("[native_x_renderer] index read past its buffer: \"{}\" ({} bytes) from {:#x}, {} {}-bit indices "
                     "(to {:#x}), index buffer type {}, VS {:016X}",
                     index_buffer->GetDesc().Name ? index_buffer->GetDesc().Name : "?", size, index_offset,
                     primitives.host_draw_vertex_count, index_type == VT_UINT16 ? 16 : 32, end,
                     int(primitives.index_buffer_type), vertex_shader ? vertex_shader->ucode_data_hash() : 0);
      }
    }
  }
  // HudMode::kOversize: the stretched pass only outside the box (the bars at the sides, or above and below), so every
  // pixel is drawn once. Over the box as well, a part transparent draw covered the middle twice: the loading screen's
  // fade to black (a Flash rectangle from -1.5 to 1.5) went dark in the 16:9 middle first and the sides lagged (the
  // user, 22 Nov 2026, at 3440x1440: "the black fade transitions are still bound to 16:9"; run uw_b21: sides 30,
  // middle 9). A scope's mask looks as before: in the box its boxed pass drew over the stretched one anyway.
  std::array<Rect, 4> bars{};
  uint32_t bar_count = 0;
  if (oversize) {
    const Rect& s = stretched_scissor;
    const int32_t bl = host_x(0), br = host_x(sized->guest_width), bt = host_y(0), bb = host_y(sized->guest_height);
    auto add = [&](int32_t l, int32_t t, int32_t r, int32_t b) {
      Rect bar{std::max(l, s.left), std::max(t, s.top), std::min(r, s.right), std::min(b, s.bottom)};
      if (bar.right > bar.left && bar.bottom > bar.top) bars[bar_count++] = bar;
    };
    add(s.left, s.top, bl, s.bottom);                                // left of the box
    add(br, s.top, s.right, s.bottom);                               // right of it
    add(std::max(bl, s.left), s.top, std::min(br, s.right), bt);     // above it
    add(std::max(bl, s.left), bb, std::min(br, s.right), s.bottom);  // below it
  }
  recorder.Do([index_buffer, index_offset, index_type, count = primitives.host_draw_vertex_count, oversize,
               stretched_viewport, bars, bar_count, viewport, scissor_rect, blend_factors,
               stencil = state.stencil_enable, stencil_reference](IDeviceContext* context) {
    if (stencil) context->SetStencilRef(stencil_reference);
    context->SetBlendFactors(blend_factors);
    if (index_buffer) context->SetIndexBuffer(index_buffer, index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    auto issue = [&] {
      if (index_buffer) {
        DrawIndexedAttribs attribs{count, index_type, DRAW_FLAG_NONE};
        context->DrawIndexed(attribs);
      } else {
        DrawAttribs attribs{count, DRAW_FLAG_NONE};
        context->Draw(attribs);
      }
    };
    if (oversize) {
      // Stretched in the bars (its black reaches the screen's edges), then in the box (its hole round).
      context->SetViewports(1, &stretched_viewport, 0, 0);
      for (uint32_t i = 0; i < bar_count; ++i) {
        context->SetScissorRects(1, &bars[i], 0, 0);
        issue();
      }
      context->SetViewports(1, &viewport, 0, 0);
      context->SetScissorRects(1, &scissor_rect, 0, 0);
    }
    issue();
  });
  // NXR_SKIN_3P_LOG=1 (a developer's): draws that are not the first person weapon's but have its colour texture:
  // another player's gun, or one on the ground. Each pair of shaders once, with its size and its first constants.
  {
    static const bool log_3p = [] {
      const char* v = std::getenv("NXR_SKIN_3P_LOG");
      return v && *v == '1';
    }();
    if (log_3p && skin_3p_hit_ && pixel_shader) {
      static std::set<uint64_t> said;
      const uint64_t key = vertex_shader->ucode_data_hash() * 31 + pixel_shader->ucode_data_hash() + primitives.guest_draw_vertex_count;
      if (said.size() < 40 && said.insert(key).second) {
        const float* c = reinterpret_cast<const float*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X]);
        std::string rows;
        for (int r : {12, 13, 14, 15, 17, 18, 19, 20, 21, 22})
          rows += fmt::format(" c{} {:.3f} {:.3f} {:.3f} {:.3f};", r, c[r * 4], c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3]);
        REXLOG_INFO("[native_x_renderer] skin 3p: a draw with the first person gun's colour texture: VS {:016X} PS {:016X}, {} "
                    "indices, {} vertex constants, depth write {}, c255.x {:.1f};{}",
                    vertex_shader->ucode_data_hash(), pixel_shader->ucode_data_hash(), primitives.guest_draw_vertex_count,
                    vertex_registers_.size(), int(state.depth_write), c[255 * 4], rows);
      }
    }
  }
  // A developer's export of the first person weapons' models (weapon_export.h, NXR_WEAPON_EXPORT): this draw's
  // vertices and indices from the game's memory, its bones from its constants, its textures. Not the hands' (told as
  // SkinView tells them: a texture of 512 by 256, or three square DXT1 ones of 512).
  if (weapon_draw_ && pixel_shader && output_sized && state.depth_write && WeaponExportOn()) {
    bool hands = false;
    uint32_t squares = 0;
    std::vector<WeaponExportDraw::Texture> textures;
    for (const ExportTexture& t : export_textures_) {
      const auto f = regs.GetTextureFetch(t.fetch);
      const uint32_t width = f.size_2d.width + 1, height = f.size_2d.height + 1;
      if (width == 512 && height == 256) hands = true;
      if (f.format == xenos::TextureFormat::k_DXT1 && width == 512 && height == 512) ++squares;
      textures.push_back({t.fetch, width, height, uint32_t(f.format), static_cast<ITextureView*>(t.view)});
    }
    auto* kernel = rex::system::kernel_state();
    const auto& bindings = vertex_shader->vertex_bindings();
    if (!hands && squares < 3 && kernel && kernel->memory() && !bindings.empty()) {
      const auto& binding = bindings[0];
      const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
      WeaponExportDraw d;
      d.vs_hash = vertex_shader->ucode_data_hash();
      d.ps_hash = pixel_shader->ucode_data_hash();
      d.vs_constants = reinterpret_cast<const float*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X]);
      d.vertex_address = fetch.address << 2;
      d.vertices = kernel->memory()->TranslatePhysical<const uint8_t*>(d.vertex_address);
      d.vertex_bytes = fetch.size << 2;
      d.stride_bytes = binding.stride_words * 4;
      for (const auto& attribute : binding.attributes) {
        const auto& a = attribute.fetch_instr.attributes;
        const int at = a.offset * 4;
        if (a.data_format == xenos::VertexFormat::k_32_32_32_FLOAT && d.position < 0) d.position = at;
        else if (a.data_format == xenos::VertexFormat::k_16_16_FLOAT && d.uv < 0) d.uv = at;
        else if (a.data_format == xenos::VertexFormat::k_8_8_8_8 && a.is_integer && d.bone < 0) d.bone = at;
        // (Of the three packed vectors the vertex shader 574AF3705A349158 reads, the normal is the one a word
        // before the last: its tangents are at words 5 and 7.)
        else if (a.data_format == xenos::VertexFormat::k_10_11_11 && at == 24) d.normal = at;
      }
      d.index_count = primitives.guest_draw_vertex_count;
      d.index_32 = primitives.host_index_format != xenos::IndexFormat::kInt16;
      d.primitive = uint32_t(primitives.guest_primitive_type);
      if (primitives.index_buffer_type == rg::PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
        d.index_address = primitives.guest_index_base;
        d.indices = kernel->memory()->TranslatePhysical<const uint8_t*>(d.index_address);
      }
      d.textures = textures.data();
      d.texture_count = uint32_t(textures.size());
      if (d.indices || primitives.index_buffer_type == rg::PrimitiveProcessor::ProcessedIndexBufferType::kNone)
        WeaponExportDrawNow(d);
    }
  }
  // The player's weapon skin (weapon_skin.h): a draw that took a skinned copy is kept, to be drawn again with the
  // skin's light (DrawSkinOver). Not with MSAA (its target would have to be multisampled as the depth is).
  if (skin_tex_now_ && output_sized && state.depth_enable && state.samples <= 1 && !vertex_registers_.empty()) {
    KeepSkinDraw(*vertex_shader, vertex_modification.value, state, viewport, scissor_rect, index_buffer, index_offset,
                 index_type, primitives.host_draw_vertex_count);
  }
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
      static const bool lamp_dump = std::getenv("NXR_MODEL_LAMP_DUMP") != nullptr;  // (read once, D247)
      if (dumped < 2 && lamp_dump) {
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
  // D222: a part drawn only while a building stands (its roof, by its index count): where it is, from the rigid
  // matrices among its vertex shader's constants (its world matrix's register is not known for every shader, so the
  // renderer matches the places against the level's buildings).
  if (intact_counts_ && intact_report_ && index_buffer && output_sized && state.depth_write &&
      std::binary_search(intact_counts_->begin(), intact_counts_->end(), primitives.guest_draw_vertex_count)) {
    float found[12];
    uint32_t n = 0;
    for (uint32_t first = 16; first + 3 <= 48 && n < 4; ++first) {
      float w[12];
      std::memcpy(w, &regs[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + first * 4], sizeof(w));
      bool rigid = true;
      for (int r = 0; r < 3 && rigid; ++r)
        for (int q = 0; q < 3 && rigid; ++q) {
          const float d = w[r * 4] * w[q * 4] + w[r * 4 + 1] * w[q * 4 + 1] + w[r * 4 + 2] * w[q * 4 + 2];
          rigid = std::abs(d - (r == q ? 1.0f : 0.0f)) < 0.02f;
        }
      if (!rigid || !(std::abs(w[3]) < 1e5f && std::abs(w[7]) < 1e5f && std::abs(w[11]) < 1e5f)) continue;
      found[n * 3] = w[3], found[n * 3 + 1] = w[7], found[n * 3 + 2] = w[11];
      ++n;
      first += 2;  // (its three rows)
    }
    if (n) intact_report_(primitives.guest_draw_vertex_count, found, n);
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
      // The world's depth range (the motion blur's): of a draw far enough to be the world's (not the weapon's view).
      GtaoProjection offered;
      if (GtaoProjection::FromMatrix(CameraVote::MatrixAt(vertex_constants_.data(), matrix, transposed), offered) &&
          offered.far_plane() > 20.0f) {
        vote_z_[0] = viewport_info.z_min, vote_z_[1] = viewport_info.z_max;
      }
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
  // D190: the sea's draws, for its reflection's mask (flat ground at the sea's height is not the sea: Guadalcanal's
  // base shone as puddles from the plane alone, run glass_fac).
  if (own_sea_reflection_ && pixel_shader) {
    if (IsSeaShader(pixel_shader->ucode_data_hash())) {
      static uint32_t seen = 0, skipped = 0;
      if (output_sized && bound_[0]) {
        sea_after_due_ = true;
        sea_after_source_ = bound_[0]->GetTexture();
      }
      static uint64_t said_at = 0;
      ++seen;
      if (output_sized && vertex_shader->GetTextureBindingsAfterTranslation().empty())
        KeepGlassDraw(*vertex_shader, vertex_modification.value, state, viewport, scissor_rect, index_buffer, index_offset,
                      index_type, primitives.host_draw_vertex_count, true);
      else
        ++skipped;
      if (device_->frames_presented() >= said_at + 1800) {
        said_at = device_->frames_presented();
        REXLOG_INFO("[native_x_renderer] sea reflection: {} of the sea's draws seen, {} not kept (another size or vertex "
                    "textures)", seen, skipped);
        seen = skipped = 0;
      }
    }
  }
  // D191: the window glass (shaders_beach/glass/glass_t01's two shaders), for its reflection's mask.
  if (own_glass_reflection_ && pixel_shader &&
      (pixel_shader->ucode_data_hash() == 0xA98B19DAA168DDAEull || pixel_shader->ucode_data_hash() == 0x5FD03DF368EEF02Eull)) {
    static uint32_t seen = 0, not_sized = 0, vertex_textures = 0;
    static uint64_t said_at = 0;
    ++seen;
    if (!output_sized) ++not_sized;
    else if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) ++vertex_textures;
    else
      KeepGlassDraw(*vertex_shader, vertex_modification.value, state, viewport, scissor_rect, index_buffer, index_offset,
                    index_type, primitives.host_draw_vertex_count);
    if (device_->frames_presented() >= said_at + 1800) {
      said_at = device_->frames_presented();
      REXLOG_INFO("[native_x_renderer] glass reflection: {} panes' draws seen, {} not in the view's size, {} with vertex "
                  "textures; VS {:016X}, target {}x{}, view {}x{}; the camera at {:.1f} {:.1f} {:.1f}", seen, not_sized,
                  vertex_textures, vertex_shader->ucode_data_hash(), sized->guest_width, sized->guest_height,
                  jitter_size_[0], jitter_size_[1], world_camera_[0], world_camera_[1], world_camera_[2]);
      seen = not_sized = vertex_textures = 0;
    }
  }
  return true;
}

namespace {
// D191: the glass mask's pixel shader: the pane's depth.
RefCntAutoPtr<IShader> CreateGlassPixelShader(IRenderDevice* device) {
  // (NXR_GLASS_DEBUG=2: 1 wherever a pane is drawn, a check of its coverage.)
  const char* dbg = std::getenv("NXR_GLASS_DEBUG");
  std::string source = std::string("#version 450\n") + "layout(location = 0) out float out_depth;\n" +
                       (dbg && (*dbg == '2' || *dbg == '4') ? "void main() { out_depth = 1.0; }\n" : "void main() { out_depth = gl_FragCoord.z; }\n");
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv = CompileGlsl(GlslStage::kPixel, source, "NativeXRenderer glass mask PS");
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = "NativeXRenderer glass mask PS";
  info.Desc.ShaderType = SHADER_TYPE_PIXEL;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}
}  // namespace

void DrawPath::KeepGlassDraw(rg::SpirvShader& vertex_shader, uint64_t vertex_modification, const PipelineState& state,
                             const Viewport& viewport, const Rect& scissor, IBuffer* index_buffer, uint64_t index_offset,
                             VALUE_TYPE index_type, uint32_t vertex_count, bool sea) {
  std::vector<GlassDraw>& draws = sea ? sea_draws_ : glass_draws_;
  std::vector<uint8_t>& kept = sea ? sea_data_ : glass_data_;
  if (!glass_pixel_shader_ && !glass_pixel_shader_failed_) {
    glass_pixel_shader_ = CreateGlassPixelShader(device_->device());
    if (!glass_pixel_shader_) {
      glass_pixel_shader_failed_ = true;
      REXLOG_ERROR("[native_x_renderer] glass reflection: the mask's pixel shader could not be created");
    }
  }
  if (!glass_pixel_shader_ || draws.size() >= 4096) return;
  bool pending = false;
  IShader* mask_vertex_shader = shaders_->GetMotionShader(vertex_shader, vertex_modification, pending);
  static uint32_t no_vs = 0, not_ready = 0;
  static uint64_t said_at = 0;
  if (device_->frames_presented() >= said_at + 1800) {
    said_at = device_->frames_presented();
    REXLOG_INFO("[native_x_renderer] glass reflection: kept draws without a mask vertex shader {} (pending {}), "
                "pipeline not ready {}", no_vs, pending, not_ready);
    no_vs = not_ready = 0;
  }
  if (!mask_vertex_shader) {
    ++no_vs;
    return;
  }
  // Into the mask: no depth (the reflection's pass tests the pane against the world's depth: on foot the depth at the
  // view's end holds only the weapon), the nearest pane kept (the depth grows toward the camera: the largest).
  PipelineState mask = state;
  mask.vertex_shader = mask_vertex_shader;
  mask.pixel_shader = glass_pixel_shader_.RawPtr();
  std::fill(std::begin(mask.rtv_formats), std::end(mask.rtv_formats), uint16_t(0));
  mask.rtv_formats[0] = uint16_t(TEX_FORMAT_R32_FLOAT);
  mask.render_target_count = 1;
  mask.dsv_format = uint16_t(TEX_FORMAT_UNKNOWN);
  mask.samples = 1;
  mask.depth_enable = 0;
  mask.depth_write = 0;
  mask.depth_func = uint8_t(COMPARISON_FUNC_ALWAYS);
  if (sea) {
    // The sea: where its own depth is what the view's depth holds (the game's sea runs on under the island, its depth
    // test hiding it there: drawn untested, Guadalcanal's base was the sea, run seamask_a). Not with MSAA (the mask
    // would have to be multisampled too).
    if (state.samples > 1) return;
    // (Not equal alone: far off the depth drawn again missed what was kept by a rounding in scattered pixels, and the
    // horizon's sea showed the game's blue there, run shore_on2. At or a hair nearer: a bias of a few of the depth's
    // least steps toward the eye; the depth grows toward the camera.)
    mask.depth_enable = 1;
    mask.depth_func = uint8_t(COMPARISON_FUNC_GREATER_EQUAL);
    mask.depth_bias = 8;
    mask.slope_scaled_depth_bias = 0.0f;
    mask.dsv_format = state.dsv_format;
    mask.samples = state.samples;
  }
  // (NXR_GLASS_DEBUG=4: neither culled nor clipped by depth, a check of where its triangles land at all.)
  static const bool loose = [] {
    const char* v = std::getenv("NXR_GLASS_DEBUG");
    return v && *v == '4';
  }();
  if (loose && !sea) mask.cull_mode = uint8_t(CULL_MODE_NONE), mask.depth_clip = 0;
  mask.stencil_enable = 0;
  mask.stencil_read_mask = mask.stencil_write_mask = 0;
  std::memset(mask.stencil_front, 0, sizeof(mask.stencil_front));
  std::memset(mask.stencil_back, 0, sizeof(mask.stencil_back));
  std::memset(mask.targets, 0, sizeof(mask.targets));
  mask.targets[0].blend_enable = 1;
  mask.targets[0].src = mask.targets[0].src_alpha = uint8_t(BLEND_FACTOR_ONE);
  mask.targets[0].dst = mask.targets[0].dst_alpha = uint8_t(BLEND_FACTOR_ONE);
  mask.targets[0].op = mask.targets[0].op_alpha = uint8_t(BLEND_OPERATION_MAX);
  mask.targets[0].write_mask = uint8_t(COLOR_MASK_RED);
  bool created = false;
  Pipeline* pipeline = pipelines_->Get(mask, vertex_shader, nullptr, &created, true);
  if (created) ++pipelines_unsaved_, frames_since_created_ = 0;
  if (!pipeline || !pipelines_->Ready(*pipeline)) {
    ++not_ready;
    return;
  }
  GlassDraw draw{pipeline, kept.size(), uint32_t(vertex_constants_.size()), viewport, scissor,
                 index_buffer, index_offset, index_type, vertex_count};
  const rg::RegisterFile& regs = *registers_;
  const auto* system = reinterpret_cast<const uint8_t*>(&system_constants_);
  const auto* bool_loop = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]);
  const auto* fetch = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0]);
  kept.insert(kept.end(), vertex_constants_.begin(), vertex_constants_.end());
  kept.insert(kept.end(), system, system + sizeof(system_constants_));
  kept.insert(kept.end(), bool_loop, bool_loop + kBoolLoopConstantsSize);
  kept.insert(kept.end(), fetch, fetch + kFetchConstantsSize);
  draws.push_back(draw);
}

ITexture* DrawPath::sea_after() const {
  return sea_after_ && sea_after_frame_ == device_->frames_presented() ? sea_after_.RawPtr() : nullptr;
}

void DrawPath::TakeSeaAfter() {
  sea_after_due_ = false;
  RefCntAutoPtr<ITexture> source = std::move(sea_after_source_);
  if (!source) return;
  const TextureDesc& d = source->GetDesc();
  if (d.SampleCount > 1) return;  // (as the sea's mask: not with MSAA)
  if (!sea_after_ || sea_after_->GetDesc().Width != d.Width || sea_after_->GetDesc().Height != d.Height ||
      sea_after_->GetDesc().Format != d.Format) {
    sea_after_.Release();
    TextureDesc copy;
    copy.Name = "NativeXRenderer the picture after the sea";
    copy.Type = RESOURCE_DIM_TEX_2D;
    copy.Width = d.Width, copy.Height = d.Height;
    copy.Format = d.Format;
    copy.BindFlags = BIND_SHADER_RESOURCE;
    device_->device()->CreateTexture(copy, nullptr, &sea_after_);
    if (!sea_after_) return;
  }
  device_->recorder().Do([source, to = sea_after_](IDeviceContext* context) {
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    CopyTextureAttribs copy(source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, to, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture(copy);
  });
  TargetsChanged();
  sea_after_frame_ = device_->frames_presented();
}

uint32_t DrawPath::DrawGlassMask(ITextureView* mask, bool sea, ITextureView* depth) {
  std::vector<GlassDraw>& draws = sea ? sea_draws_ : glass_draws_;
  std::vector<uint8_t>& kept = sea ? sea_data_ : glass_data_;
  uint32_t drawn = 0;
  if (mask && !draws.empty()) {
    Recorder& recorder = device_->recorder();
    if (sea && !depth) {
      draws.clear();
      kept.clear();
      return 0;
    }
    recorder.Do([mask, depth = sea ? depth : nullptr](IDeviceContext* context) mutable {
      context->SetRenderTargets(1, &mask, depth, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    });
    TargetsChanged();
    uploaded_frame_ = UINT64_MAX;
    memory_->PrepareForRead();
    for (const GlassDraw& draw : draws) {
      const uint8_t* data = kept.data() + draw.data;
      Pipeline& pipeline = *draw.pipeline;
      {
        MappedBuffer map(recorder, pipeline.float_constants[0]);
        std::memcpy(map.data(), data, draw.constants_size);
      }
      {
        MappedBuffer map(recorder, pipeline.previous_float_constants);
        std::memcpy(map.data(), data, draw.constants_size);
      }
      data += draw.constants_size;
      auto upload = [&](IBuffer* buffer, size_t size) {
        MappedBuffer map(recorder, buffer);
        std::memcpy(map.data(), data, size);
        data += size;
      };
      upload(system_constants_buffer_, sizeof(system_constants_));
      upload(bool_loop_constants_buffer_, kBoolLoopConstantsSize);
      upload(fetch_constants_buffer_, kFetchConstantsSize);
      GlassDraw at = draw;
      static const bool loose = [] {
        const char* v = std::getenv("NXR_GLASS_DEBUG");
        return v && *v == '4';
      }();
      if (loose && !sea) {
        at.viewport.MinDepth = 0.0f, at.viewport.MaxDepth = 1.0f;
        at.scissor = Rect{0, 0, 16384, 16384};
      }
      recorder.Do([pso = pipeline.pso.RawPtr(), srb = pipeline.srb.RawPtr(), draw = at](IDeviceContext* context) {
        context->SetPipelineState(pso);
        context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
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
      });
      ++drawn;
    }
  }
  {
    static uint64_t said_at = 0;
    static const uint64_t every = std::getenv("NXR_GLASS_DEBUG") ? 240 : 1800;
    if (drawn && !sea && device_->frames_presented() >= said_at + every) {
      said_at = device_->frames_presented();
      REXLOG_INFO("[native_x_renderer] glass reflection: {} panes' draws in the mask this frame", drawn);
      if (std::getenv("NXR_GLASS_DEBUG") && !last_glass_.empty()) {  // (D191: where its first draw lands)
        const GlassDraw& g = last_glass_.front();
        REXLOG_INFO("[native_x_renderer] glass reflection: first draw's viewport {:.0f} {:.0f} {:.0f}x{:.0f} depth {:.3f}..{:.3f}, "
                    "scissor {} {} {} {}, {} vertices, indices {} at {}",
                    g.viewport.TopLeftX, g.viewport.TopLeftY, g.viewport.Width, g.viewport.Height, g.viewport.MinDepth,
                    g.viewport.MaxDepth, g.scissor.left, g.scissor.top, g.scissor.right, g.scissor.bottom, g.vertex_count,
                    g.index_buffer == memory_->buffer() ? "guest" : g.index_buffer ? "host" : "none", g.index_offset);
        if (!last_glass_data_.empty()) {
          const float* c = reinterpret_cast<const float*>(last_glass_data_.data());
          const size_t n = std::min<size_t>(g.constants_size / 4, 16);
          std::string row;
          for (size_t i = 0; i < n; ++i) row += fmt::format(" {:.3f}", c[i]);
          REXLOG_INFO("[native_x_renderer] glass reflection: {} bytes of vertex constants, the first:{}", g.constants_size, row);
        }
      }
    }
  }
  if (!sea && !draws.empty()) {
    last_glass_.assign(draws.begin(), draws.begin() + 1);
    last_glass_data_.assign(kept.begin() + draws[0].data, kept.begin() + draws[0].data + draws[0].constants_size);
  }
  draws.clear();
  kept.clear();
  return drawn;
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
    Recorder& recorder = device_->recorder();
    recorder.Do([motion, depth](IDeviceContext* context) mutable {
      context->SetRenderTargets(1, &motion, depth, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    });
    TargetsChanged();
    uploaded_frame_ = UINT64_MAX;  // its own bool/loop and fetch constants go into the draws' buffers (D81)
    memory_->PrepareForRead();
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
        MappedBuffer map(recorder, buffer);
        std::memcpy(map.data(), data, size);
        data += size;
      };
      upload(pipeline.float_constants[0], draw.constants_size);
      {
        MappedBuffer map(recorder, pipeline.previous_float_constants);
        std::memcpy(map.data(), draw.previous_constants, draw.constants_size);
      }
      upload(system_constants_buffer_, sizeof(system_constants_));
      upload(bool_loop_constants_buffer_, kBoolLoopConstantsSize);
      upload(fetch_constants_buffer_, kFetchConstantsSize);
      {
        const float ndc_to_pixels[4] = {0.5f * draw.viewport.Width, -0.5f * draw.viewport.Height, 0.0f, 0.0f};
        MappedBuffer map(recorder, motion_constants_buffer_);
        std::memcpy(map.data(), ndc_to_pixels, sizeof(ndc_to_pixels));
      }
      recorder.Do([pso = pipeline.pso.RawPtr(), srb = pipeline.srb.RawPtr(), draw](IDeviceContext* context) {
        context->SetPipelineState(pso);
        context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
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
      });
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

void DrawPath::KeepSkinDraw(rg::SpirvShader& vertex_shader, uint64_t vertex_modification, const PipelineState& state,
                            const Viewport& viewport, const Rect& scissor, IBuffer* index_buffer, uint64_t index_offset,
                            VALUE_TYPE index_type, uint32_t vertex_count) {
  if (skin_over_failed_ || skin_draws_.size() >= 64) return;
  if (!skin_over_shader_) {
    skin_over_shader_ = CreateWeaponSkinOverShader(device_->device());
    SamplerDesc linear;
    linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_WRAP;
    device_->device()->CreateSampler(linear, &skin_sampler_);
    if (!skin_over_shader_ || !skin_sampler_) {
      skin_over_failed_ = true;
      REXLOG_ERROR("[native_x_renderer] weapon skin: the mesh pass's pixel shader could not be created; no light of its own");
      return;
    }
  }
  ITextureView* plain = WeaponSkinPlainView(skin_tex_now_);
  if (!plain) return;
  // The gun's place from its own bones (the vertex shader 574AF3705A349158: a bone's rows are c[17 + 3 * bone] and the
  // two after, in the world's space; c12 to c15 its view-projection).
  {
    const float* c = reinterpret_cast<const float*>(&(*registers_)[rg::XE_GPU_REG_SHADER_CONSTANT_000_X]);
    const float step = c[255 * 4];
    const int first = 17 + int(std::lround(step * float(WeaponSkinBodyBone())));
    const int magazine = 17 + int(std::lround(step * float(WeaponSkinMagazineBone())));
    double view_move[16];
    const bool moved = WeaponLayerMoveNow(view_move);
    if (step > 0.5f && step < 8.5f && first + 2 < 255 && magazine + 2 < 255)
      WeaponSkinGunFrame(c + first * 4, c + magazine * 4, c + 12 * 4, weapon_px_, weapon_py_, moved ? view_move : nullptr);
  }
  // The draw's own vertex shader (the motion pass's copy of it: it gives the same positions and the game's own
  // interpolators), the skin's pixel shader; depth tested equal to what the draw wrote, not written; no stencil, no
  // blending; one target.
  bool pending = false;
  IShader* own_vertex_shader = shaders_->GetMotionShader(vertex_shader, vertex_modification, pending);
  if (!own_vertex_shader) return;
  PipelineState over = state;
  over.vertex_shader = own_vertex_shader;
  over.pixel_shader = skin_over_shader_.RawPtr();
  std::fill(std::begin(over.rtv_formats), std::end(over.rtv_formats), uint16_t(0));
  over.rtv_formats[0] = uint16_t(TEX_FORMAT_RGBA16_FLOAT);
  over.render_target_count = 1;
  over.depth_write = 0;
  over.depth_func = uint8_t(COMPARISON_FUNC_EQUAL);
  over.stencil_enable = 0;
  over.stencil_read_mask = over.stencil_write_mask = 0;
  std::memset(over.stencil_front, 0, sizeof(over.stencil_front));
  std::memset(over.stencil_back, 0, sizeof(over.stencil_back));
  std::memset(over.targets, 0, sizeof(over.targets));
  over.targets[0].write_mask = uint8_t(COLOR_MASK_ALL);
  Pipeline* pipeline = pipelines_->Get(over, vertex_shader, nullptr, nullptr, true);
  if (!pipeline || !pipelines_->Ready(*pipeline)) return;
  const TextureDesc& size = plain->GetTexture()->GetDesc();
  SkinDraw draw{pipeline, skin_data_.size(), uint32_t(vertex_constants_.size()), viewport, scissor, index_buffer, index_offset,
                index_type, vertex_count, RefCntAutoPtr<ITextureView>(plain), float(size.Width), float(size.Height)};
  const rg::RegisterFile& regs = *registers_;
  const auto* system = reinterpret_cast<const uint8_t*>(&system_constants_);
  const auto* bool_loop = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031]);
  const auto* fetch = reinterpret_cast<const uint8_t*>(&regs[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0]);
  skin_data_.insert(skin_data_.end(), vertex_constants_.begin(), vertex_constants_.end());
  skin_data_.insert(skin_data_.end(), system, system + sizeof(system_constants_));
  skin_data_.insert(skin_data_.end(), bool_loop, bool_loop + kBoolLoopConstantsSize);
  skin_data_.insert(skin_data_.end(), fetch, fetch + kFetchConstantsSize);
  skin_draws_.push_back(std::move(draw));
  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("[native_x_renderer] weapon skin: the gun's draws are drawn again with the storm's light (VS {:016X})",
                vertex_shader.ucode_data_hash());
  }
}

uint32_t DrawPath::DrawSkinOver(ITextureView* target, ITextureView* depth) {
  uint32_t drawn = 0;
  if (target) {
    Recorder& recorder = device_->recorder();
    // (A multisampled depth cannot be bound with the target: none kept then, the target only cleared.)
    const bool tested = depth && depth->GetTexture()->GetDesc().SampleCount <= 1 &&
                        depth->GetTexture()->GetDesc().Width == target->GetTexture()->GetDesc().Width &&
                        depth->GetTexture()->GetDesc().Height == target->GetTexture()->GetDesc().Height;
    recorder.Do([target, depth, tested](IDeviceContext* context) mutable {
      context->SetRenderTargets(1, &target, tested ? depth : nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      context->ClearRenderTarget(target, none, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    });
    TargetsChanged();
    if (tested && !skin_draws_.empty()) {
      uploaded_frame_ = UINT64_MAX;  // its own bool/loop and fetch constants go into the draws' buffers (D81)
      memory_->PrepareForRead();
      for (const SkinDraw& draw : skin_draws_) {
        const uint8_t* data = skin_data_.data() + draw.data;
        Pipeline& pipeline = *draw.pipeline;
        auto upload = [&](IBuffer* buffer, size_t size) {
          MappedBuffer map(recorder, buffer);
          std::memcpy(map.data(), data, size);
          data += size;
        };
        {
          MappedBuffer map(recorder, pipeline.previous_float_constants);  // (the motion copy's: not used by the skin)
          std::memcpy(map.data(), data, draw.constants_size);
        }
        upload(pipeline.float_constants[0], draw.constants_size);
        upload(system_constants_buffer_, sizeof(system_constants_));
        upload(bool_loop_constants_buffer_, kBoolLoopConstantsSize);
        upload(fetch_constants_buffer_, kFetchConstantsSize);
        {
          float k[16] = {};
          WeaponSkinOverConstants(k + 4, draw.width, draw.height);
          MappedBuffer map(recorder, motion_constants_buffer_);
          std::memcpy(map.data(), k, sizeof(k));
        }
        recorder.Do([pso = pipeline.pso.RawPtr(), srb = pipeline.srb.RawPtr(), viewport = draw.viewport, scissor = draw.scissor,
                     index_buffer = draw.index_buffer, index_offset = draw.index_offset, index_type = draw.index_type,
                     vertex_count = draw.vertex_count, texture = draw.texture.RawPtr(),
                     sampler = skin_sampler_.RawPtr()](IDeviceContext* context) {
          // skinTex is a combined image and sampler (kOverPS): no "skinTex_sampler" variable, and the pipeline (the
          // game's draws' cache) has no immutable sampler, so its sampler is the view's own. Without one the
          // descriptor's sampler was null, and NVIDIA's Vulkan driver read through it as the resources were committed
          // (TekRantGaming, 26 Nov 2026: "When it's equipped, my game crashes immediately when loading in with that
          // class"; his dump and run skin_crash1: nvoglv64.dll, a read of 0xf0, from here; other drivers let it pass).
          if (texture->GetSampler() != sampler) texture->SetSampler(sampler);
          if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "skinTex")) v->Set(texture);
          if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "skinTex_sampler")) v->Set(sampler);
          context->SetPipelineState(pso);
          context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
          context->SetViewports(1, &viewport, 0, 0);
          context->SetScissorRects(1, &scissor, 0, 0);
          if (index_buffer) {
            context->SetIndexBuffer(index_buffer, index_offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            DrawIndexedAttribs attribs{vertex_count, index_type, DRAW_FLAG_NONE};
            context->DrawIndexed(attribs);
          } else {
            DrawAttribs attribs{vertex_count, DRAW_FLAG_NONE};
            context->Draw(attribs);
          }
        });
        ++drawn;
      }
    }
    recorder.Do([](IDeviceContext* context) { context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE); });
  }
  // (Kept until the textures' references are let go on the recorder's side: the draws above hold raw pointers.)
  recorder_skin_done_.swap(skin_draws_);
  skin_draws_.clear();
  skin_data_.clear();
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
  // D197: the soft shadows' numbers (0 with them off: the game's own filter).
  std::memcpy(c.soft_shadow, soft_shadow_, sizeof(c.soft_shadow));
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

// The records' frame cameras, all 16, taken under the lock once (D248): FindAttachedMove takes no record more than
// 5 mm from the eye, so a world matrix whose eye is near none of them needs no other check (MoveAttached).
void AttachedPositions(double out[16][3]) {
  std::lock_guard lock(g_attached_mutex);
  for (size_t i = 0; i < g_attached.size(); ++i)
    for (int c = 0; c < 3; ++c) out[i][c] = g_attached[i].position[c];
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

// The first person weapon's picture moved in the view (SetWeaponLayerMove): the move, and until when it holds (the
// game sets it each frame it applies; a quarter second without one ends it).
namespace {
std::mutex g_weapon_move_mutex;
double g_weapon_move[16];
std::atomic<float> g_weapon_move_py{0.0f};  // 1 / tan(half the weapon view's vertical field of view)
std::chrono::steady_clock::time_point g_weapon_move_until{};
std::atomic<bool> g_weapon_move_on{false};
}  // namespace
// The move as it holds now (a view space 4 x 4, rows first), for what is drawn with the weapon but not by its own
// draws (the weapon skin's cloud): false without one.
bool WeaponLayerMoveNow(double out[16]) {
  if (!g_weapon_move_on.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_weapon_move_mutex);
  if (std::chrono::steady_clock::now() > g_weapon_move_until) return false;
  for (int i = 0; i < 16; ++i) out[i] = g_weapon_move[i];
  return true;
}
void RecordWeaponLayerMove(const float* move, float fov_degrees) {
  std::lock_guard lock(g_weapon_move_mutex);
  if (!move) {
    g_weapon_move_on.store(false, std::memory_order_relaxed);
    return;
  }
  for (int i = 0; i < 16; ++i) g_weapon_move[i] = move[i];
  g_weapon_move_py.store(1.0f / std::tan(std::clamp(fov_degrees, 1.0f, 170.0f) * 0.5f * 3.14159265f / 180.0f), std::memory_order_relaxed);
  g_weapon_move_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  g_weapon_move_on.store(true, std::memory_order_relaxed);
}
namespace {
// NXR_WEAPON_DUMP (ApplyOutdoorLight): the seconds after the first draw at which to log the weapon's draws; -1 off.
double WeaponDumpAt() {
  static const double at = [] {
    const char* v = std::getenv("NXR_WEAPON_DUMP");
    return v && *v ? std::atof(v) : -1.0;
  }();
  return at;
}
// Where each of Battlefield 1943's shaders takes its outdoor light from (tools/light_constants.py).
struct LightRegisters {
  uint64_t hash;
  uint8_t pixel, dir, hemisphere_dir, top, bottom, key, specular, camera, shadow;
  uint8_t envmap;  // 1: it samples outdoorLightReflectionEnvmap, times the specular constant's w
};
constexpr LightRegisters kLightRegisters[] = {
#include "bf_lights.inc"
};
// D241: the pixel shaders that draw the ground, the terrain's and the ground patches' (tools/ground_shaders.py).
struct GroundEntry {
  uint64_t hash;
  uint8_t kind;  // 1 the terrain's (heightmap), 2 a ground patch's
};
constexpr GroundEntry kGroundShaders[] = {
#include "bf_ground.inc"
};
// 0: not the ground's; 1: the terrain's; 2: a ground patch's, or the terrain's tyre-track pass (D243: 0365AC9437C0240E
// and 0A7847F161DA00A7, the same samplers on another level), which the game's own sun lights (D244).
uint8_t GroundKind(uint64_t hash) {
  if (hash == 0x0365AC9437C0240Eull || hash == 0x0A7847F161DA00A7ull) return 2;
  const GroundEntry* end = std::end(kGroundShaders);
  const GroundEntry* it = std::lower_bound(std::begin(kGroundShaders), end, hash,
                                           [](const GroundEntry& e, uint64_t h) { return e.hash < h; });
  return it != end && it->hash == hash ? it->kind : 0;
}
// D182: what is left of a reflected day's sky at this hour: the painted sky's factor (the same picture of an
// afternoon), its luminance; 1 at noon, about 0.09 at night. NXR_SKY_REFLECTION_KEEP=1 (tests): 1, as the game has it.
float SkyReflectionLeft(const float* painted) {
  static const bool keep = [] {
    const char* v = std::getenv("NXR_SKY_REFLECTION_KEEP");
    return v && *v == '1';
  }();
  return keep ? 1.0f : std::max(0.0f, 0.2126f * painted[0] + 0.7152f * painted[1] + 0.0722f * painted[2]);
}
}  // namespace

namespace {
// The rotation's basis for a direction: its columns the direction itself (toward the light), the horizontal to its
// right and the third.
void LightBasis(double azimuth, double elevation, double out[3][3]) {
  const double ce = std::cos(elevation), se = std::sin(elevation), ca = std::cos(azimuth), sa = std::sin(azimuth);
  const double f[3] = {ce * sa, se, ce * ca}, r[3] = {ca, 0.0, -sa}, u[3] = {-se * sa, ce, -se * ca};
  for (int i = 0; i < 3; ++i) out[i][0] = f[i], out[i][1] = r[i], out[i][2] = u[i];
}
bool Invert3(const double a[3][3], double inv[3][3]) {
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::abs(det) < 1e-30) return false;
  inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
  inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
  inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
  inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
  inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
  inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
  inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
  inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
  inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
  return true;
}
// Battlefield 1943's shadow pass (runs shad1 and shad2, Wake Island): a 1024 x 1024 depth target with no colour.
bool IsShadowPass(const uint32_t* regs) {
  return regs[0x2082] == 0x04000400 && (regs[0x2000] & 0x3FFF) == 0x410 && regs[0x2001] == 0;
}
}  // namespace

void DrawPath::SetOutdoorLight(const OutdoorLight& light) {
  outdoor_light_ = light;
  // The turn once a frame, before its first draw: within a frame the casters and what receives their shadows must
  // get the same one (this is called before every draw).
  const uint64_t frame = device_->frames_presented();
  if (frame == shadow_turn_frame_ && shadow_turn_frame_valid_) return;
  shadow_turn_frame_ = frame, shadow_turn_frame_valid_ = true;
  LatchShadowTurn();
}

// The game's shadows turned to another sun (14 Nov 2026, "Yes, move the sun"). What the game does (run shad2): three
// cascades (on Wake Island 28, 79 and 370 m wide), each drawn in turn into the shadow pass's target with a clip
// matrix L (world to clip, orthographic, in the casters' c0 to c3, as it is or times the object's world matrix), and
// what receives shadows gets diag(0.5, -0.5, 1) L of each in its vertex shader. Its shadows fall from a steeper sun
// than the one it shades with (Wake Island: 63 degrees up against 35).
//
// For a sun elsewhere the world is turned about a pivot p near the camera before the game's matrix: L' = L M with
// M x = p + R (x - p), R taking the new direction of the light to the game's. In clip space that is one matrix for
// a cascade, C = T(q) A R A^-1 T(-q) with A the linear part of L and q = L p, so L' = C L: with the pivot chosen by
// its clip coordinates (the cascade's middle, at the camera's depth of the last frame) C needs nothing of this
// frame's, and the casters' matrices (C times theirs) and the receivers' (diag C diag^-1 times theirs) stay each
// other's exactly. The game still chooses what casts shadows for its own sun: what it leaves out casts none.
// D239: the sky's sun given to the game's own shadow setup (ShadowSourceDirection, the game side's hook): what it needs
// of the latched light, and until when the game is being fed (the turn then stays off).
namespace {
std::mutex g_sun_source_mutex;
struct SunSource {
  bool valid = false;
  double sun_azimuth = 0, sun_elevation = 0, steep = 0, light_azimuth = 0, light_elevation = 0;
} g_sun_source;
std::atomic<int64_t> g_sun_source_fed_until{0};
int64_t SunSourceNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

bool ShadowSourceDirection(const float game_ray[3], float out_ray[3]) {
  SunSource s;
  {
    std::lock_guard lock(g_sun_source_mutex);
    s = g_sun_source;
  }
  if (!s.valid) return false;
  const double kPi = 3.14159265358979;
  // The game's own shadow ray points from the sun to the ground: its sun is the other way.
  const double from[3] = {-game_ray[0], -game_ray[1], -game_ray[2]};
  const double len = std::sqrt(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]);
  if (!(len > 1e-6)) return false;
  const double shadow_elevation = std::asin(std::clamp(from[1] / len, -1.0, 1.0));
  const double shadow_azimuth = std::atan2(from[0], from[2]);
  // As the turn did (D181): the shadows keep the game's lead over its light, between 40 and 75 degrees up, steep in rain;
  // round to the sky's sun.
  double elevation = std::clamp(shadow_elevation + (s.sun_elevation - s.light_elevation), 40.0 * kPi / 180.0, 75.0 * kPi / 180.0);
  elevation += (80.0 * kPi / 180.0 - elevation) * std::clamp(s.steep, 0.0, 1.0);
  const double azimuth = shadow_azimuth + std::remainder(s.sun_azimuth - s.light_azimuth, 2.0 * kPi);
  const double ce = std::cos(elevation);
  out_ray[0] = float(-ce * std::sin(azimuth));
  out_ray[1] = float(-std::sin(elevation));
  out_ray[2] = float(-ce * std::cos(azimuth));
  g_sun_source_fed_until.store(SunSourceNow() + 500, std::memory_order_relaxed);
  return true;
}

void DrawPath::LatchShadowTurn() {
  shadow_turn_.on = false;
  shadow_turn_.dir[3] = 0.0f;  // 1 once the direction is set
  const OutdoorLight& light = outdoor_light_;
  {  // D239: what the game's shadow setup needs for the sky's sun (ShadowSourceDirection)
    std::lock_guard lock(g_sun_source_mutex);
    g_sun_source.valid = light.on && light.turn && game_light_dir_valid_ && REXCVAR_GET(native_x_renderer_sun_source);
    if (g_sun_source.valid) {
      const double d[3] = {game_light_dir_[0], game_light_dir_[1], game_light_dir_[2]};
      const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      g_sun_source.valid = len > 1e-9;
      if (g_sun_source.valid) {
        g_sun_source.light_elevation = std::asin(std::clamp(d[1] / len, -1.0, 1.0));
        g_sun_source.light_azimuth = std::atan2(d[0], d[2]);
        g_sun_source.sun_azimuth = light.azimuth, g_sun_source.sun_elevation = light.elevation;
        g_sun_source.steep = light.steep;
      }
    }
  }
  // While the game is fed the sky's sun its shadows fall from it already: nothing to turn, only the light's direction.
  if (light.on && light.turn && SunSourceNow() < g_sun_source_fed_until.load(std::memory_order_relaxed)) {
    const double ce = std::cos(light.elevation);
    shadow_turn_.dir[0] = float(ce * std::sin(light.azimuth));
    shadow_turn_.dir[1] = float(std::sin(light.elevation));
    shadow_turn_.dir[2] = float(ce * std::cos(light.azimuth));
    shadow_turn_.dir[3] = 1.0f;
    return;
  }
  if (!light.on || !light.turn || !seen_shadow_valid_ || !seen_camera_valid_ || !game_light_dir_valid_) return;
  const double kPi = 3.14159265358979;
  auto angles = [](const double d[3], double& azimuth, double& elevation) {
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-12) return false;
    elevation = std::asin(std::clamp(d[1] / len, -1.0, 1.0));
    azimuth = std::atan2(d[0], d[2]);
    return true;
  };
  double clip[3][3][4];
  for (int i = 0; i < 3; ++i)
    for (int c = 0; c < 4; ++c) {
      clip[i][0][c] = 2.0 * seen_shadow_[i][0][c];
      clip[i][1][c] = -2.0 * seen_shadow_[i][1][c];
      clip[i][2][c] = seen_shadow_[i][2][c];
    }
  // Where the game's shadows fall from: what the first cascade's x and y do not see.
  const double* x = clip[0][0];
  const double* y = clip[0][1];
  double from[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
  if (from[1] < 0.0) from[0] = -from[0], from[1] = -from[1], from[2] = -from[2];
  const double game_light[3] = {game_light_dir_[0], game_light_dir_[1], game_light_dir_[2]};
  double shadow_az, shadow_el, light_az, light_el;
  if (!angles(from, shadow_az, shadow_el) || !angles(game_light, light_az, light_el)) return;
  {  // NXR_SHADOW_LOG: the game's own shadow and light directions, every 600 frames (D239 research)
    static uint64_t dir_said_at = 0;
    static const bool log = std::getenv("NXR_SHADOW_LOG") != nullptr;
    if (log && device_->frames_presented() >= dir_said_at + 600) {
      dir_said_at = device_->frames_presented();
      REXLOG_INFO("[native_x_renderer] shadow log: the game's shadows from {:.1f} up, azimuth {:.1f}; its light from {:.1f}, "
                  "{:.1f}", shadow_el * 57.29578, shadow_az * 57.29578, light_el * 57.29578, light_az * 57.29578);
    }
  }
  double turn_az = std::remainder(double(light.azimuth) - light_az, 2.0 * kPi);
  double turn_el = double(light.elevation) - light_el;
  // D218: from 60 to 150 m over the ground or the sea the turn fades out, and higher up the shadows fall as the game
  // casts them (the user, 19 Nov 2026, from a Corsair's chase view a few hundred metres over Wake Island: "the ground
  // shadows dislocate from their objects and follow the plane camera"; not reproduced from parked or gliding
  // cameras on the four maps, runs hs_* and vote_*). The game chooses what casts shadows, and fits its cascades, for
  // its own sun; seen from high up, its own direction is the one that always matches them. The light on what is lit
  // stays the sky's sun's. On foot, on a deck or a roof and in low flight nothing changes.
  {
    double keep = 1.0;
    if (!std::isnan(sea_level_)) {
      float floor_height = sea_level_, ground_here = 0.0f;
      if (native_x_renderer::GrassCameraGround(seen_camera_[0], seen_camera_[2], &ground_here))
        floor_height = std::max(floor_height, ground_here);
      const double t = std::clamp((double(seen_camera_[1] - floor_height) - 60.0) / 90.0, 0.0, 1.0);
      keep = 1.0 - t * t * (3.0 - 2.0 * t);
    }
    turn_az *= keep, turn_el *= keep;
    shadow_turn_keep_ = float(keep);
  }
  // The shading's direction always; the shadows only when they are turned at all (the game's own hour stays the
  // game's own picture to the last bit).
  const double ce = std::cos(light.elevation);
  shadow_turn_.dir[0] = float(ce * std::sin(light.azimuth));
  shadow_turn_.dir[1] = float(std::sin(light.elevation));
  shadow_turn_.dir[2] = float(ce * std::cos(light.azimuth));
  shadow_turn_.dir[3] = 1.0f;
  if (std::abs(turn_az) < 0.002 && std::abs(turn_el) < 0.002) return;
  // The shadows keep their lead over the light, but not flatter than 40 degrees (the cascades are only so deep)
  // and not steeper than 75 (at noon nothing would cast a shadow to see).
  double new_el = std::clamp(shadow_el + turn_el, 40.0 * kPi / 180.0, 75.0 * kPi / 180.0);
  new_el += (80.0 * kPi / 180.0 - new_el) * std::clamp(double(light.steep), 0.0, 1.0);
  double game_basis[3][3], new_basis[3][3], rotation[3][3];
  LightBasis(shadow_az, shadow_el, game_basis);
  LightBasis(shadow_az + turn_az, new_el, new_basis);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      rotation[r][c] = game_basis[r][0] * new_basis[c][0] + game_basis[r][1] * new_basis[c][1] + game_basis[r][2] * new_basis[c][2];
  const double d[3] = {0.5, -0.5, 1.0};
  // D205: the ground under the camera (the grass's height map, asked a few frames ago): the pivot a metre over it,
  // never above the camera. Without an answer (no grass, a level loading) the camera's depth as before.
  // NXR_SHADOW_PIVOT_CAMERA=1 (a diagnosis aid): the camera's depth, as before.
  static const bool pivot_camera = std::getenv("NXR_SHADOW_PIVOT_CAMERA") != nullptr;
  float ground = 0.0f;
  // Never under the sea (the height map is the terrain's, the sea floor too: 187.7 m under the US carrier's deck at
  // 230, run sh_dusk_ground), so a sea level must be known.
  const bool have_ground = !pivot_camera && !std::isnan(sea_level_) &&
                           native_x_renderer::GrassCameraGround(seen_camera_[0], seen_camera_[2], &ground);
  // Within 20 m of the ground (on foot, on a deck or a roof, the ground under a deck being the sea 17 m down) the
  // camera's depth as before; from 20 to 40 m up it moves to the ground.
  const float floor_height = std::max(ground, sea_level_);
  const float up = seen_camera_[1] - floor_height;
  const float to_ground = std::clamp((up - 20.0f) / 20.0f, 0.0f, 1.0f);
  const float pivot_height = std::min(floor_height + 1.0f, seen_camera_[1]);
  const bool use_ground = have_ground && to_ground > 0.0f;
  {
    static uint64_t said_at = 0;
    static const bool log = std::getenv("NXR_SHADOW_LOG") != nullptr;
    if (log && device_->frames_presented() >= said_at + 600) {
      said_at = device_->frames_presented();
      REXLOG_INFO("[native_x_renderer] shadow log: the turn's pivot {} (the camera at {:.1f} {:.1f} {:.1f}); the turn kept "
                  "{:.2f} (D218)",
                  use_ground ? fmt::format("toward the ground, {:.1f} m, by {:.2f}", pivot_height, to_ground)
                             : std::string("at the camera's depth"),
                  seen_camera_[0], seen_camera_[1], seen_camera_[2], shadow_turn_keep_);
    }
  }
  for (int i = 0; i < 3; ++i) {
    double a[3][3], inv[3][3], ar[3][3], g[3][3];
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) a[r][c] = clip[i][r][c];
    if (!Invert3(a, inv)) return;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) ar[r][c] = a[r][0] * rotation[0][c] + a[r][1] * rotation[1][c] + a[r][2] * rotation[2][c];
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) g[r][c] = ar[r][0] * inv[0][c] + ar[r][1] * inv[1][c] + ar[r][2] * inv[2][c];
    double depth = clip[i][2][0] * seen_camera_[0] + clip[i][2][1] * seen_camera_[1] + clip[i][2][2] * seen_camera_[2] +
                   clip[i][2][3];
    // D205: the pivot on the ground, where the cascade's middle line meets it, not at the camera's depth (the user,
    // 19 Nov 2026: flying over buildings "their shadows gets skewed or stuck to the players camera ... and project over
    // the terrain as you fly"). From a plane the camera's depth is a point in the air: the turned cascades then cover
    // other ground than the game chose its casters for, and the buildings cast nothing (run sh_fly_true: the airfield's
    // houses without shadows 60 m up, with them as the game turns them, sh_fly_false). On the ground the two are a
    // metre or two apart. The middle line is clip x = y = 0; its height at clip depth z is linear in z.
    if (use_ground) {
      const double t0 = clip[i][0][3], t1 = clip[i][1][3], t2 = clip[i][2][3];
      if (std::abs(inv[1][2]) > 1e-9) {
        const double at = t2 + (double(pivot_height) + inv[1][0] * t0 + inv[1][1] * t1) / inv[1][2];
        if (std::isfinite(at)) depth += (at - depth) * double(to_ground);
      }
    }
    // The turned depth is put about the map's middle and halved (D181): the game keeps the camera near the far end of
    // each cascade's depth (Wake Island's deck: 0.93 of the first's 167 m), where its own sun needs no more, and the
    // turn tilts the depth across the map (up to a third of the range in the widest cascade): the ground and what
    // stands on it then left the map's depth on one side of the view or another, and its shadows went or all of it
    // was in shadow, by where the camera looked. The casters' and the receivers' depths get the same change.
    const double kDepthScale = 0.5;
    for (int c = 0; c < 3; ++c) g[2][c] *= kDepthScale;
    for (int r = 0; r < 3; ++r) {
      const double move = (r == 2 ? 0.5 : 0.0) - g[r][2] * depth;
      for (int c = 0; c < 3; ++c) {
        shadow_turn_.caster[i][r][c] = float(g[r][c]);
        shadow_turn_.receiver[i][r][c] = float(g[r][c] * d[r] / d[c]);
        shadow_turn_.inverse[i][r][c] = float(inv[r][c]);
      }
      shadow_turn_.caster[i][r][3] = float(move);
      shadow_turn_.receiver[i][r][3] = float(move * d[r]);
    }
  }
  shadow_turn_.on = true;
  {
    static const bool log = std::getenv("NXR_SHADOW_LOG") != nullptr;
    static uint64_t said_at = 0;
    if (log && device_->frames_presented() >= said_at + 1200) {
      said_at = device_->frames_presented();
      for (int i = 0; i < 3; ++i) {
        auto norm = [&](int r) { return std::sqrt(clip[i][r][0] * clip[i][r][0] + clip[i][r][1] * clip[i][r][1] + clip[i][r][2] * clip[i][r][2]); };
        const double cz = clip[i][2][0] * seen_camera_[0] + clip[i][2][1] * seen_camera_[1] + clip[i][2][2] * seen_camera_[2] + clip[i][2][3];
        const double cx = clip[i][0][0] * seen_camera_[0] + clip[i][0][1] * seen_camera_[1] + clip[i][0][2] * seen_camera_[2] + clip[i][0][3];
        const double cy = clip[i][1][0] * seen_camera_[0] + clip[i][1][1] * seen_camera_[1] + clip[i][1][2] * seen_camera_[2] + clip[i][1][3];
        REXLOG_INFO("[native_x_renderer] shadow log: cascade {}: {:.1f} m across, {:.1f} m high, {:.1f} m deep; the camera at clip "
                    "{:.3f} {:.3f} {:.3f}; turned z row {:.4f} {:.4f} {:.4f}", i, 2.0 / norm(0), 2.0 / norm(1), 1.0 / norm(2), cx, cy, cz,
                    shadow_turn_.caster[i][2][0], shadow_turn_.caster[i][2][1], shadow_turn_.caster[i][2][2]);
      }
    }
  }
  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("[native_x_renderer] outdoor light: the game's shadows fall from {:.1f} degrees up, azimuth {:.1f}; its "
                "light from {:.1f}, {:.1f}; turned by {:.1f} around and {:.1f} up",
                shadow_el * 180.0 / kPi, shadow_az * 180.0 / kPi, light_el * 180.0 / kPi, light_az * 180.0 / kPi,
                turn_az * 180.0 / kPi, turn_el * 180.0 / kPi);
  }
}

// The outdoor light's constants of a draw's shader, changed in its uploaded constants (SetOutdoorLight): the
// colours multiplied, the direction replaced, the shadow pass's and the shadow receivers' matrices turned. The
// values are read from the register file: the mapped buffer is slow to read back.
void DrawPath::ApplyOutdoorLight(uint64_t shader_hash, uint32_t stage, const uint32_t* regs, uint32_t first,
                                 const uint64_t* float_bitmap, uint8_t* mapped) {
  auto used = [&](uint32_t reg) { return reg < 255 && (float_bitmap[reg >> 6] & (uint64_t(1) << (reg & 63))); };
  if (!stage) draw_shadow_valid_ = false;
  // NXR_WEAPON_DUMP=<seconds> (a diagnosis aid, 19 Nov 2026: do the shadows and the day's light reach the first
  // person arms and gun?): on two frames from that many seconds after the first draw, each stage of every draw
  // ClassifyWeapon takes for the weapon's: its shader, its light table row, and the float constants c0 to c11 (where
  // the game's shadow receivers keep their three cascades' transforms).
  if (WeaponDumpAt() >= 0.0 && weapon_draw_) {
    static const auto since = std::chrono::steady_clock::now();
    static uint64_t first_frame = 0;
    const uint64_t now_frame = device_->frames_presented();
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count() >= WeaponDumpAt()) {
      if (!first_frame) first_frame = now_frame;
      if (now_frame < first_frame + 2) {
        const LightRegisters* end = std::end(kLightRegisters);
        const LightRegisters* row = std::lower_bound(std::begin(kLightRegisters), end, shader_hash,
                                                     [](const LightRegisters& a, uint64_t h) { return a.hash < h; });
        const bool listed = row != end && row->hash == shader_hash && row->pixel == stage;
        std::string line = fmt::format("[native_x_renderer] weapon draw: frame {} {} {:016X} table {}", now_frame,
                                       stage ? "PS" : "VS", shader_hash,
                                       listed ? fmt::format("dir {} key {} shadow {}", row->dir, row->key, row->shadow)
                                              : std::string("none"));
        for (uint32_t reg = 0; reg < 12; ++reg) {
          if (!used(reg)) continue;
          float f[4];
          std::memcpy(f, &regs[first + (reg << 2)], sizeof(f));
          line += fmt::format(" c{}=({:.4f} {:.4f} {:.4f} {:.2f})", reg, f[0], f[1], f[2], f[3]);
        }
        REXLOG_INFO("{}", line);
      }
    }
  }
  auto at = [&](uint32_t reg) {
    uint32_t index = uint32_t(std::popcount(float_bitmap[reg >> 6] & ((uint64_t(1) << (reg & 63)) - 1)));
    for (uint32_t i = 0; i < (reg >> 6); ++i) index += uint32_t(std::popcount(float_bitmap[i]));
    return mapped + size_t(index) * 16;
  };
  // Three rows of a matrix (three registers from `reg`) multiplied from the left by a 3 x 4 one, its fourth row
  // 0, 0, 0, 1 as the rows' own.
  auto turn = [&](uint32_t reg, const float by[3][4]) {
    float rows[3][4], out[4];
    std::memcpy(rows, &regs[first + (reg << 2)], sizeof(rows));
    for (uint32_t r = 0; r < 3; ++r) {
      if (!used(reg + r)) continue;
      for (int c = 0; c < 4; ++c) out[c] = by[r][0] * rows[0][c] + by[r][1] * rows[1][c] + by[r][2] * rows[2][c];
      out[3] += by[r][3];
      std::memcpy(at(reg + r), out, sizeof(out));
    }
  };
  // A caster in the shadow pass: its clip matrix is in c0 to c3 (viewProjMatrix or worldViewProjMatrix in every
  // shader of the pass). Its cascade is the one whose linear part leaves a rotation with one scale of the matrix's.
  if (!stage && shadow_turn_.on && IsShadowPass(regs) && used(0) && used(1) && used(2)) {
    float rows[3][4];
    std::memcpy(rows, &regs[first], sizeof(rows));
    int best = -1;
    float best_error = 0.05f;
    for (int i = 0; i < 3; ++i) {
      float w[3][3], scale = 0.0f, error = 0.0f;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
          w[r][c] = shadow_turn_.inverse[i][r][0] * rows[0][c] + shadow_turn_.inverse[i][r][1] * rows[1][c] +
                    shadow_turn_.inverse[i][r][2] * rows[2][c];
          scale += w[r][c] * w[r][c];
        }
      scale /= 3.0f;
      if (scale < 1e-12f) continue;
      for (int r = 0; r < 3; ++r)
        for (int c = r; c < 3; ++c) {
          const float dot = (w[r][0] * w[c][0] + w[r][1] * w[c][1] + w[r][2] * w[c][2]) / scale;
          error = std::max(error, std::abs(dot - (r == c ? 1.0f : 0.0f)));
        }
      if (error < best_error) best_error = error, best = i;
    }
    if (best >= 0) turn(0, shadow_turn_.caster[best]);
    {  // NXR_SHADOW_LOG (a developer's check): the casters no cascade was found for (they are drawn unturned)
      static const bool log = std::getenv("NXR_SHADOW_LOG") != nullptr;
      static std::map<uint64_t, uint32_t> missed;
      static uint64_t turned = 0, said_at = 0;
      if (log) {
        if (best < 0) ++missed[shader_hash];
        else ++turned;
        const uint64_t frame = device_->frames_presented();
        if (frame >= said_at + 1200) {
          said_at = frame;
          std::string line;
          for (const auto& [h, n] : missed) line += fmt::format(" {:016X}x{}", h, n);
          REXLOG_INFO("[native_x_renderer] shadow log: {} casters turned; not turned:{}", turned, line.empty() ? " none" : line);
          REXLOG_INFO("[native_x_renderer] shadow log: last unmatched rows {:.4f} {:.4f} {:.4f} {:.2f} | {:.4f} {:.4f} {:.4f} {:.2f} | "
                      "{:.4f} {:.4f} {:.4f} {:.2f}", last_miss_[0], last_miss_[1], last_miss_[2], last_miss_[3], last_miss_[4],
                      last_miss_[5], last_miss_[6], last_miss_[7], last_miss_[8], last_miss_[9], last_miss_[10], last_miss_[11]);
          missed.clear(), turned = 0;
        }
        if (best < 0) std::memcpy(last_miss_, rows, sizeof(last_miss_));
      }
    }
    return;
  }
  // The game's painted sky by the hour: its dome, the dome's half size copy, the tint over it and its cloud sprites
  // (the shaders SetSkipGameSky names) each scale their colour by colorScale, pixel constant 0.
  // The breakable window glass (the material shaders_beach/glass/glass_t01: the air tower, guard towers, factory,
  // warehouse, lighthouse; the user, 17 Nov 2026: "any building with a breakable glass window" reflects daylight at
  // night): its two shaders take no outdoor light at all. Each reflects a cube map of a day's sky (sampler_Texture19)
  // with a dirt texture and multiplies the colour by colorScale.x, pixel constant 0, and nothing else (the
  // disassembly of A98B19DAA168DDAE; alpha, the pane's see-through, is not scaled): that x dimmed as D182's.
  if (stage && outdoor_light_.on && used(0) &&
      (shader_hash == 0xA98B19DAA168DDAEull || shader_hash == 0x5FD03DF368EEF02Eull)) {
    float v[4];
    std::memcpy(v, &regs[first], sizeof(v));
    // D191: none of the day's cube map while the renderer reflects on the glass itself (its colour is that alone).
    v[0] *= own_glass_reflection_ ? 0.0f : SkyReflectionLeft(outdoor_light_.painted);
    std::memcpy(at(0), v, sizeof(v));
    return;
  }
  if (stage && outdoor_light_.on && outdoor_light_.game_sky && used(0) &&
      (shader_hash == 0x8ABF56776060E64Cull || shader_hash == 0xE059F66662ECF0A4ull ||
       shader_hash == 0x471C5A0705715DB7ull || shader_hash == 0x10FA030CD9308DE2ull ||
       shader_hash == 0xE20CB2628975A77Dull)) {
    float v[4];
    std::memcpy(v, &regs[first], sizeof(v));
    v[0] *= outdoor_light_.painted[0], v[1] *= outdoor_light_.painted[1], v[2] *= outdoor_light_.painted[2];
    std::memcpy(at(0), v, sizeof(v));
    return;
  }
  // D195: the effects (the user, 18 Nov 2026: "Explosion, bullet, water traversal (running through the water) and
  // smoke effects are lit from the map's default lighting, resulting in them looking blinding white at night and in
  // the storm"). The game's particle shaders take no outdoor light: their light is in their colour, made for the day.
  // They are the pixel shaders with colorScale (c0) and the depth buffer (soft particles) and no outdoor light, the
  // same on every level (tools/light_constants.py's tables; the sky's dome, its half size copy and the painted cloud
  // sprites, which share the signature, are dealt with above). What they blend over the picture (smoke, dust, spray)
  // takes the light the sky gives now: colorScale's colour times a mix of the sun's (a third) and the sky's (two
  // thirds) light; what they add (fire, flashes, tracers: their own light) is left as it is.
  if (stage && outdoor_light_.on && used(0) &&
      (shader_hash == 0x0F79C07341B8A6DAull || shader_hash == 0x2787E655836200FEull ||
       shader_hash == 0x56764E04420DC7EFull || shader_hash == 0x66C95D39154FF344ull ||
       shader_hash == 0x92E4718A17BA7DE8ull || shader_hash == 0xCB44050787464809ull ||
       shader_hash == 0xDE60E17F72CE8181ull || shader_hash == 0xE20CB2628975A77Dull)) {
    const uint32_t blend = regs[rg::XE_GPU_REG_RB_BLENDCONTROL0];
    const uint32_t src = blend & 0x1F, dst = (blend >> 8) & 0x1F;
    const bool adds = dst == 1;                 // ONE: light of its own
    const bool over = !(src == 1 && dst == 0);  // blended at all
    if (over && !adds) {
      float v[4];
      std::memcpy(v, &regs[first], sizeof(v));
      for (int c = 0; c < 3; ++c)
        v[c] *= std::clamp(0.35f * outdoor_light_.key[c] + 0.65f * outdoor_light_.sky[c], 0.0f, 1.5f);
      std::memcpy(at(0), v, sizeof(v));
    }
    return;
  }
  const LightRegisters* end = std::end(kLightRegisters);
  const LightRegisters* r = std::lower_bound(std::begin(kLightRegisters), end, shader_hash,
                                             [](const LightRegisters& a, uint64_t h) { return a.hash < h; });
  const bool listed = r != end && r->hash == shader_hash && r->pixel == stage;
  // D218: a vertex shader the light table does not list (its runtime name cannot be made from the game's files, only
  // recorded in a run: those seen only from the air were missing) that receives the sun's shadows still gets the
  // turn. The user, 19 Nov 2026: "the ground shadows dislocate from their objects and follow the plane camera" (from a
  // plane over Wake Island: the island's shadows out on the sea). Such a receiver read the turned map with the game's
  // own transforms, so what it showed of it was in the wrong place, and moved as the cascades follow the camera. Every
  // receiver of a frame gets the same three transforms: this frame's, as a listed receiver got them, are looked for
  // among its constants (the exact values, all three cascades' rows at +0, +4, +8); where they are is kept for that
  // shader, and turned from then on (the turn is in the cascades' clip space, right with an object's own matrix in
  // them too).
  static const bool unlisted_off = std::getenv("NXR_SHADOW_UNLISTED_OFF") != nullptr;  // (a diagnosis aid: as before)
  if (!stage && shadow_turn_.on && !(listed && r->shadow < 245) && !unlisted_off) {
    uint32_t reg = 255;
    if (auto it = unlisted_receivers_.find(shader_hash); it != unlisted_receivers_.end()) {
      reg = it->second;
    } else if (frame_shadow_frame_ == device_->frames_presented()) {
      for (uint32_t q = 0; q + 10 < 255 && reg == 255; ++q) {
        if (!used(q) || std::memcmp(&regs[first + (q << 2)], frame_shadow_[0][0], 16) != 0) continue;
        bool all = true;
        for (uint32_t i = 0; i < 3 && all; ++i)
          for (uint32_t row = 0; row < 3 && all; ++row)
            all = used(q + 4 * i + row) && std::memcmp(&regs[first + ((q + 4 * i + row) << 2)], frame_shadow_[i][row], 16) == 0;
        if (all) reg = q;
      }
      if (reg != 255) {
        unlisted_receivers_[shader_hash] = reg;
        REXLOG_INFO("[native_x_renderer] shadows: a receiver the light table does not list, vertex shader {:016X}: its "
                    "cascades' transforms at c{}, turned from now on (D218)", shader_hash, reg);
      }
    }
    if (reg != 255 && used(reg) && used(reg + 8u))
      for (uint32_t i = 0; i < 3; ++i) turn(reg + 4 * i, shadow_turn_.receiver[i]);
  }
  if (!listed) return;
  if (stage && used(r->dir)) {
    std::memcpy(game_light_dir_, &regs[first + (uint32_t(r->dir) << 2)], sizeof(game_light_dir_));
    game_light_dir_valid_ = true;
    // NXR_SHADOW_LOG: the game's light direction as its shader got it, its bits (research, D239: where the game keeps
    // it, to give it the sky's sun at the source).
    // (Read once: getenv walks the environment block, and this runs at every lit draw: 14 % of the renderer's thread
    // while unset, D247.)
    static const bool shadow_log = std::getenv("NXR_SHADOW_LOG") != nullptr;
    static bool said_dir = false;
    if (!said_dir && shadow_log) {
      said_dir = true;
      uint32_t bits[4];
      std::memcpy(bits, game_light_dir_, sizeof(bits));
      REXLOG_INFO("[native_x_renderer] shadow log: the game's light direction {:.5f} {:.5f} {:.5f} {:.5f} (bits {:08X} {:08X} "
                  "{:08X} {:08X})", game_light_dir_[0], game_light_dir_[1], game_light_dir_[2], game_light_dir_[3],
                  bits[0], bits[1], bits[2], bits[3]);
    }
  }
  // The cascades' transforms and the camera's place are taken together from one draw (its vertex shader's and its
  // pixel shader's): the first person weapon's are in a space of its own with the camera at 0, and a pair from
  // two spaces put the pivot hundreds of metres off (runs sun3 to sun8: the shadows gone).
  // The camera's place in the world, from any lit draw of the world's (not the first person weapon's, whose camera is
  // at 0): kept for a view with no terrain in it (WorldCamera).
  if (stage && used(r->camera)) {
    float eye[3];
    std::memcpy(eye, &regs[first + (uint32_t(r->camera) << 2)], sizeof(eye));
    if (std::abs(eye[0]) + std::abs(eye[1]) + std::abs(eye[2]) > 1.0f && std::isfinite(eye[0] + eye[1] + eye[2])) {
      std::memcpy(world_camera_, eye, sizeof(world_camera_));
      world_camera_frame_ = device_->frames_presented(), world_camera_valid_ = true;
    }
  }
  if (stage && used(r->camera) && draw_shadow_valid_) {
    std::memcpy(seen_camera_, &regs[first + (uint32_t(r->camera) << 2)], sizeof(seen_camera_));
    std::memcpy(seen_shadow_, draw_shadow_, sizeof(seen_shadow_));
    seen_camera_valid_ = seen_shadow_valid_ = true;
    // The shadow map for the rain's cover, once a frame, from a draw of the world's (its camera is not at 0): the
    // transforms as this draw's vertex shader got them (turned, if the shadows are), the view when it is bound.
    const uint64_t frame = device_->frames_presented();
    if (shadow_map_.frame != frame &&
        std::abs(seen_camera_[0]) + std::abs(seen_camera_[1]) + std::abs(seen_camera_[2]) > 1.0f) {
      for (int i = 0; i < 3; ++i)
        for (int row = 0; row < 3; ++row)
          for (int col = 0; col < 4; ++col) {
            float v = draw_shadow_[i][row][col];
            if (shadow_turn_.on) {
              const float (*by)[4] = shadow_turn_.receiver[i];
              v = by[row][0] * draw_shadow_[i][0][col] + by[row][1] * draw_shadow_[i][1][col] +
                  by[row][2] * draw_shadow_[i][2][col] + (col == 3 ? by[row][3] : 0.0f);
            }
            shadow_map_.cascades[i][row][col] = v;
          }
      shadow_map_pending_ = true;
      UpdateSoftShadows();  // D197
    }
  }
  // What receives shadows: the three cascades' transforms, three rows each.
  if (!stage && r->shadow < 245 && used(r->shadow) && used(r->shadow + 4u) && used(r->shadow + 8u)) {
    for (uint32_t i = 0; i < 3; ++i)
      std::memcpy(draw_shadow_[i], &regs[first + ((uint32_t(r->shadow) + 4 * i) << 2)], sizeof(draw_shadow_[i]));
    draw_shadow_valid_ = true;
    std::memcpy(frame_shadow_, draw_shadow_, sizeof(frame_shadow_));  // D218
    frame_shadow_frame_ = device_->frames_presented();
    if (shadow_turn_.on)
      for (uint32_t i = 0; i < 3; ++i) turn(uint32_t(r->shadow) + 4 * i, shadow_turn_.receiver[i]);
  }
  // The wet weapon shines ("almost glass-like"): the sun's specular colour its shaders get is made stronger by how
  // wet it is (up to three times), on top of what the day's light makes of it.
  auto wet_shine = [&] {
    if (!stage || !(weapon_draw_ || vehicle_draw_) || !used(r->specular) || wet_count_ * wet_show_ <= 0.01f) return;
    float v[4];
    std::memcpy(v, at(r->specular), sizeof(v));
    const float more = 1.0f + 2.0f * wet_count_ * wet_show_;
    v[0] *= more, v[1] *= more, v[2] *= more;
    std::memcpy(at(r->specular), v, sizeof(v));
  };
  if (!outdoor_light_.on) {
    wet_shine();
    return;
  }
  auto scale = [&](uint8_t reg, const float* by) {
    if (!used(reg)) return;
    float v[4];
    std::memcpy(v, &regs[first + (uint32_t(reg) << 2)], sizeof(v));
    v[0] *= by[0], v[1] *= by[1], v[2] *= by[2];
    std::memcpy(at(reg), v, sizeof(v));
  };
  // D245: the sea as the game lights it (the user, 24 Nov 2026: "tune the water color closer to the SDK look at the
  // same time of day", then "I thought we already agreed on the game's sun for this"): Wake Island's open sea at 10:30
  // was (71, 125, 191) under the hour's light and reflected sky, the SDK's (18, 76, 109), and with the game's painted
  // sky alone (49, 103, 152) (runs sea4_w_ours, sea4_w_sdk, sea2_seasky0). Its light keeps the game's direction and
  // colours, all scaled by one factor, how bright the hour's sun is (its key colour's luminance, at most 1), and it
  // reflects the game's own painted sky as dimmed for the hour (SkyReflectionLeft).
  const bool sea_game = stage && IsSeaShader(shader_hash) && REXCVAR_GET(native_x_renderer_sea_game_light);
  float sea_by[3] = {1.0f, 1.0f, 1.0f};
  if (sea_game) {
    const float* k = outdoor_light_.key;
    const float g = std::clamp(0.2126f * k[0] + 0.7152f * k[1] + 0.0722f * k[2], 0.0f, 1.0f);
    sea_by[0] = sea_by[1] = sea_by[2] = g;
  }
  scale(r->key, sea_game ? sea_by : outdoor_light_.key);
  scale(r->specular, sea_game ? sea_by : outdoor_light_.key);
  // Window glass, water and shiny metal reflect a cube map of a day's sky, times the specular constant's w (the
  // user, 17 Nov 2026: "Building windows with glass are reflecting daylight when it's night"): that w dimmed as the
  // game's painted sky is (the same picture of an afternoon), about a tenth of it left at night.
  if (r->envmap && used(r->specular)) {
    float v[4];
    std::memcpy(v, at(r->specular), sizeof(v));
    // D190: the sea's shaders (their water textures, sampler_Diffuse_Water2 with the depth and a distortion: two a
    // level) reflect nothing of the cube map while the renderer's own reflection is on (the fog pass's).
    const bool sea = shader_hash == 0x60BD629ABC6BFCFCull || shader_hash == 0xBEAD02FECCE1E2EAull ||
                     shader_hash == 0x2715129AEB1C7AE7ull || shader_hash == 0xED75062BA2E66F5Dull ||
                     shader_hash == 0xC7F420106FAD9628ull || shader_hash == 0xFEFCD04010C7CA86ull ||
                     shader_hash == 0x1139C2856EADF277ull || shader_hash == 0xF819F26C2C47D01Bull;
    // D240: without it, the sea's cube map is the renderer's sky of the hour (SeaSkyView), already as dark as the
    // hour's sky: not dimmed for the hour, but 0.6 of it (whole, the sea by day was a pale glare of the horizon's
    // colour: run hz_after_1050).
    const float was = v[3], by = sea && own_sea_reflection_ ? 0.0f
                               : sea && sea_game           ? SkyReflectionLeft(outdoor_light_.painted)  // D245
                               : sea && own_sea_sky_       ? 0.6f
                                                           : SkyReflectionLeft(outdoor_light_.painted);
    v[3] *= by;
    std::memcpy(at(r->specular), v, sizeof(v));
    static uint64_t said_at = 0;
    if (device_->frames_presented() >= said_at + 3000) {
      said_at = device_->frames_presented();
      REXLOG_INFO("[native_x_renderer] a sky reflection (PS {:016X}): its strength {:.3f} times {:.3f}", shader_hash, was, by);
    }
  }
  wet_shine();
  scale(r->top, sea_game ? sea_by : outdoor_light_.sky);
  scale(r->bottom, sea_game ? sea_by : outdoor_light_.ground);
  if (shadow_turn_.dir[3] != 0.0f && used(r->dir) && !sea_game) {
    float v[4];
    std::memcpy(v, &regs[first + (uint32_t(r->dir) << 2)], sizeof(v));
    v[0] = shadow_turn_.dir[0], v[1] = shadow_turn_.dir[1], v[2] = shadow_turn_.dir[2];
    // D241: the ground's relief. Its tyre ruts, footprints and ripples are in its normal maps, drawn for the game's
    // own low sun (Wake Island: 35 degrees up); under the sky's sun at noon (65 degrees at 10:30) they flattened into
    // soft patches (the report: "certain terrain texture layers, like the vehicle tracks, disappear from the player
    // camera view"; RenderDoc, 24 Nov 2026: the SDK's frame and ours drew flag C's ground with the same textures,
    // samplers and constants but this one, and with the game's light ours had the ruts too). The ground's shaders
    // (the terrain's and the ground patches', bf_ground.inc) get a sun lower toward the game's light, at its own
    // azimuth, halfway down to it by default (native_x_renderer_ground_relief_strength), and its key colour stronger
    // by as much as flat ground lost: flat ground is lit as before. Its shadows stay the sky's sun's.
    const uint8_t ground_kind = stage && used(r->key) && REXCVAR_GET(native_x_renderer_ground_relief) ? GroundKind(shader_hash) : 0;
    if (ground_kind) {
      constexpr float kDeg = 0.0174532925f;
      const float up = std::clamp(v[1], -1.0f, 1.0f), across = std::sqrt(v[0] * v[0] + v[2] * v[2]);
      float cap = 35.0f * kDeg;
      if (game_light_dir_valid_)
        cap = std::clamp(std::asin(std::clamp(game_light_dir_[1], -1.0f, 1.0f)), 25.0f * kDeg, 50.0f * kDeg);
      const float elevation = std::asin(up);
      const float strength = std::clamp(float(REXCVAR_GET(native_x_renderer_ground_relief_strength)), 0.0f, 1.0f);
      // D244: the game's own sun the whole way, the terrain's hills too: their slopes turned from it darker, as in the
      // SDK (the user, 24 Nov 2026: "I liked the hills dark, it looks closer to the SDK renderer"; half way for the
      // terrain, native_x_renderer_ground_relief_terrain_weight 0.5, they were a tenth darker, runs gs2_w05, gs3_w05).
      const bool game_sun = REXCVAR_GET(native_x_renderer_ground_relief_game_sun) && game_light_dir_valid_;
      if (game_sun && elevation > cap) {
        // The game's own light that way, blended in over 15 degrees above its height (dawn and dusk keep the hour's),
        // the key colour stronger by as much as flat ground lost, as below.
        const float w = std::clamp((elevation - cap) / (15.0f * kDeg), 0.0f, 1.0f) *
                        std::clamp(float(ground_kind == 2 ? REXCVAR_GET(native_x_renderer_ground_relief_game_sun_weight)
                                                          : REXCVAR_GET(native_x_renderer_ground_relief_terrain_weight)),
                                   0.0f, 1.0f);
        float g[3] = {game_light_dir_[0], game_light_dir_[1], game_light_dir_[2]};
        const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (gl > 1e-4f && g[1] / gl > 0.05f) {
          float d[3];
          for (int i = 0; i < 3; ++i) d[i] = v[i] + (g[i] / gl - v[i]) * w;
          const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
          if (dl > 1e-4f && d[1] / dl > 0.05f) {
            for (int i = 0; i < 3; ++i) v[i] = d[i] / dl;
            const float more = up / v[1];
            float key[4];
            std::memcpy(key, at(r->key), sizeof(key));
            key[0] *= more, key[1] *= more, key[2] *= more;
            std::memcpy(at(r->key), key, sizeof(key));
          }
        }
      } else if (elevation > cap && across > 1e-4f && strength > 0.0f) {
        // Halfway down by default: the whole way, slopes of the terrain turned from the sun went much darker (a 30
        // degree slope's light a quarter of the sky's at 65 degrees; runs rel1_true, rel2_true); halfway, 30%
        // less, and the ruts' contrast 1.8 times the sky's sun's (2.5 the whole way).
        const float lit = elevation - strength * (elevation - cap);
        v[0] = v[0] / across * std::cos(lit), v[1] = std::sin(lit), v[2] = v[2] / across * std::cos(lit);
        const float more = up / std::sin(lit);
        float key[4];
        std::memcpy(key, at(r->key), sizeof(key));
        key[0] *= more, key[1] *= more, key[2] *= more;
        std::memcpy(at(r->key), key, sizeof(key));
      }
    }
    std::memcpy(at(r->dir), v, sizeof(v));
  }
}

// Whether this draw is the first person weapon's or hands' (for their wetness). Battlefield 1943 draws what is
// within 7.5 m in a depth range of its own (the viewport's 1 to 0.0133: run wlog2), the weapon and the hands among
// it, with a field of view of their own. The world's field of view is taken once a frame from a draw of a farther
// range; a draw of the nearest range whose own differs from it by 2% or more is the weapon's.
void DrawPath::ClassifyWeapon(const uint32_t* regs, uint32_t first, const uint64_t* float_bitmap, float z_min, float z_max) {
  weapon_draw_ = false;
  skin_tex_now_ = nullptr;
  skin_3p_hit_ = false;
  export_textures_.clear();
  if (wet_tint_ < 0 && wet_count_ * wet_show_ < 0.01f && WeaponDumpAt() < 0.0 && !WeaponSkinNow() && !WeaponExportOn()) return;  // nothing to be wet, skinned or exported
  const bool nearest = z_min > 0.99f && z_max > 0.005f && z_max < 0.05f;
  const uint64_t frame = device_->frames_presented();
  const bool farther = !nearest && z_max < z_min && z_min < 0.05f && world_py_frame_ != frame;
  if (!nearest && !farther) return;
  uint8_t numbers[256];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = float_bitmap[i];
    while (bits) {
      numbers[n++] = uint8_t((i << 6) + uint32_t(std::countr_zero(bits)));
      bits &= bits - 1;
    }
  }
  if (n < 4) return;
  std::array<uint8_t, 256 * 16> values;
  for (uint32_t k = 0; k < n; ++k) std::memcpy(values.data() + k * 16, &regs[first + (uint32_t(numbers[k]) << 2)], 16);
  uint32_t index = 0;
  bool transposed = false;
  if (!CameraVote::FindMatrix(values.data(), numbers, n, index, transposed)) return;
  GtaoProjection p;
  // (A field of view between 19 and 147 degrees: other matrices are found too, run chk3.)
  if (!GtaoProjection::FromMatrix(CameraVote::MatrixAt(values.data(), index, transposed), p) || !(p.py > 0.3f && p.py < 6.0f)) {
    return;
  }
  if (farther) {
    world_py_ = p.py, world_py_frame_ = frame;
    return;
  }
  weapon_draw_ = world_py_ > 0.0f && std::abs(p.py - world_py_) > 0.02f * world_py_;
  weapon_constants_ = n;
  if (weapon_draw_) weapon_px_ = std::abs(p.px), weapon_py_ = std::abs(p.py), NoteFirstPersonWeaponDrawn();
  static int said = 0;
  if (weapon_draw_ && said < 2) {
    ++said;
    REXLOG_INFO("[native_x_renderer] wetness: a weapon's draw (its view's py {:.3f}, the world's {:.3f})", p.py, world_py_);
  }
}

void DrawPath::MoveWeaponLayer(const uint32_t* regs, uint32_t first, const uint64_t* float_bitmap, uint8_t* mapped) {
  if (!g_weapon_move_on.load(std::memory_order_relaxed)) return;
  static std::atomic<uint32_t> s_calls{0}, s_few{0}, s_nomatrix{0}, s_noproj{0};
  {
    static auto at = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - at > std::chrono::seconds(3)) {
      at = std::chrono::steady_clock::now();
      REXLOG_INFO("[native_x_renderer] weapon layer: {} draws looked at, {} with under 4 constants, {} without a perspective matrix, {} not a projection",
                  s_calls.exchange(0), s_few.exchange(0), s_nomatrix.exchange(0), s_noproj.exchange(0));
    }
  }
  ++s_calls;
  uint8_t numbers[256];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = float_bitmap[i];
    while (bits) {
      numbers[n++] = uint8_t((i << 6) + uint32_t(std::countr_zero(bits)));
      bits &= bits - 1;
    }
  }
  if (n < 4) {
    ++s_few;
    return;
  }
  std::array<uint8_t, 256 * 16> values;
  for (uint32_t k = 0; k < n; ++k) std::memcpy(values.data() + k * 16, &regs[first + (uint32_t(numbers[k]) << 2)], 16);
  uint32_t index = 0;
  bool transposed = false;
  if (!CameraVote::FindMatrix(values.data(), numbers, n, index, transposed)) {
    ++s_nomatrix;
    return;
  }
  const Mat4d w = CameraVote::MatrixAt(values.data(), index, transposed);
  GtaoProjection p;
  if (!GtaoProjection::FromMatrix(w, p)) {
    ++s_noproj;
    return;
  }
  const float near_plane = p.near_plane(), far_plane = p.far_plane();
  // The weapon's view by its field of view (the world's camera reads near 0.1 and far 7.5 here too: its depth is
  // mapped into a sliver of the range, so the planes do not tell them apart).
  const float want_py = g_weapon_move_py.load(std::memory_order_relaxed);
  if (!(std::abs(p.py - want_py) <= 0.01f * want_py) || !(near_plane > 0.0f && near_plane < 1.0f) || std::abs(p.b) < 1e-9f) {
    static std::mutex said_mutex;
    static std::set<int> said;  // each field of view once
    std::lock_guard lock(said_mutex);
    if (said.size() < 60 && said.insert(int(p.py * 40.0f) * 1000 + int(std::min(near_plane, 99.0f) * 10.0f)).second)
      REXLOG_INFO("[native_x_renderer] weapon layer: not the weapon's (py {:.3f}, the weapon's {:.3f}; near {:.4f}, far {:.2f}, px {:.3f})",
                  p.py, want_py, near_plane, far_plane, p.px);
    return;
  }
  Mat4d move;
  {
    std::lock_guard lock(g_weapon_move_mutex);
    if (std::chrono::steady_clock::now() > g_weapon_move_until) {
      g_weapon_move_on.store(false, std::memory_order_relaxed);
      return;
    }
    for (int i = 0; i < 16; ++i) move[i] = g_weapon_move[i];
  }
  // clip = P x view, with clip w = view z: P and its inverse from the projection's terms; the draw's matrix becomes
  // P x move x P^-1 x itself (the move happens in the view's space, between the view and the projection).
  const double px = p.px, py = p.py, cx = p.cx, cy = p.cy, a = p.a, b = p.b;
  const Mat4d proj = {px, 0, cx, 0, 0, py, cy, 0, 0, 0, a, b, 0, 0, 1, 0};
  const Mat4d inv = {1 / px, 0, 0, -cx / px, 0, 1 / py, 0, -cy / py, 0, 0, 0, 1, 0, 0, 1 / b, -a / b};
  auto mul = [](const Mat4d& l, const Mat4d& r) {
    Mat4d o{};
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) o[i * 4 + j] += l[i * 4 + k] * r[k * 4 + j];
    return o;
  };
  const Mat4d moved = mul(mul(mul(proj, move), inv), w);
  float out[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[transposed ? c * 4 + r : r * 4 + c] = float(moved[r * 4 + c]);
  std::memcpy(mapped + size_t(index) * 16, out, sizeof(out));
  {
    static auto at = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    if (std::chrono::steady_clock::now() - at > std::chrono::seconds(10)) {
      at = std::chrono::steady_clock::now();
      REXLOG_INFO("[native_x_renderer] weapon layer: a draw moved (near {:.3f}, far {:.2f}, px {:.3f}, py {:.3f}, block c{}{})",
                  near_plane, far_plane, p.px, p.py, numbers[index], transposed ? ", columns" : "");
    }
  }
}

void DrawPath::MoveAttached(const uint32_t* regs, uint32_t first, const uint64_t* float_bitmap, uint8_t* mapped) {
  // NXR_GUN_TRACE=<seconds>:<lines> (a diagnosis aid, D201): from that many seconds after the first draw at the eye,
  // on each frame, the screen place of the first three bones of the first six skinned models at the eye (eight or more bone
  // matrices: the first person arms and gun), to see whether it steps with the game's tick on screen.
  static const std::pair<double, int> gun_trace = [] {
    const char* v = std::getenv("NXR_GUN_TRACE");
    if (!v || !*v) return std::pair<double, int>{-1.0, 0};
    const char* colon = std::strchr(v, ':');
    return std::pair<double, int>{std::atof(v), colon ? std::atoi(colon + 1) : 2000};
  }();
  if (!g_attached_any.load(std::memory_order_relaxed) && gun_trace.first < 0) return;
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
  auto at = [&](uint32_t k) { return reinterpret_cast<const uint8_t*>(&regs[first + (uint32_t(numbers[k]) << 2)]); };
  uint32_t index = 0, found = 0;
  bool transposed = false;
  while (index + 4 <= n && !CameraVote::FindMatrix(at(index), numbers + index, 4, found, transposed)) ++index;
  if (index + 4 > n) return;
  const Mat4d a = CameraVote::MatrixAt(at(index), 0, transposed);
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
  if (gun_trace.first >= 0) {
    static const auto first_at = std::chrono::steady_clock::now();
    static uint64_t traced_frame = ~0ull;
    static int lines = 0, in_frame = 0;
    const uint64_t frame = device_->frames_presented();
    if (frame != traced_frame) in_frame = 0;
    if (in_frame < 6 && lines < gun_trace.second &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - first_at).count() >= gun_trace.first) {
      double ndc[3][2] = {};
      int bones = 0;
      for (uint32_t k = 0; k + 2 < n; ++k) {
        if (numbers[k + 2] != numbers[k] + 2 || (k + 2 >= index && k <= index + 3)) continue;
        float r[3][4];
        for (int i = 0; i < 3; ++i) std::memcpy(r[i], at(k + i), 16);
        const double len = std::sqrt(double(r[0][0]) * r[0][0] + double(r[0][1]) * r[0][1] + double(r[0][2]) * r[0][2]);
        if (!(len > 0.01 && len < 100.0)) continue;
        k += 2;  // the next bone starts after this one's three rows
        if (bones < 3) {
          const double p[4] = {r[0][3], r[1][3], r[2][3], 1.0};
          const double cx = a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3];
          const double cy = a[4] * p[0] + a[5] * p[1] + a[6] * p[2] + a[7];
          const double cw = a[12] * p[0] + a[13] * p[1] + a[14] * p[2] + a[15];
          if (std::abs(cw) > 1e-6) ndc[bones][0] = cx / cw, ndc[bones][1] = cy / cw;
        }
        ++bones;
      }
      if (bones >= 8) {
        traced_frame = frame;
        ++lines, ++in_frame;
        REXLOG_INFO("[native_x_renderer] gun trace: frame {} draw {} py {:.4f} near {:.4f} bones {} at {:.5f} {:.5f}, "
                    "{:.5f} {:.5f}, {:.5f} {:.5f}",
                    frame, in_frame, projection.py, projection.near_plane(), bones, ndc[0][0], ndc[0][1], ndc[1][0],
                    ndc[1][1], ndc[2][0], ndc[2][1]);
      }
    }
  }
  if (!g_attached_any.load(std::memory_order_relaxed)) return;
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
  // D248: the eye such rows put in the world is tested against the records first, which rejects nearly all of them
  // (a soldier's bones, every triplet of every model near the eye: their checks, a 4 x 4 inverse, a lock and a clock
  // read each, were 1.5 to 2 % of the renderer's thread). FindAttachedMove still decides, as before.
  double recorded[16][3];
  AttachedPositions(recorded);
  for (uint32_t k = 0; k + 2 < n; ++k) {
    if (numbers[k + 2] != numbers[k] + 2) continue;
    if (k + 2 >= index && k <= index + 3) continue;
    float r[3][4];
    for (int i = 0; i < 3; ++i) std::memcpy(r[i], at(k + i), 16);
    // As rows of world = W x position (column form), the eye in the world.
    double world_eye[3];
    for (int i = 0; i < 3; ++i) world_eye[i] = r[i][0] * eye[0] + r[i][1] * eye[1] + r[i][2] * eye[2] + r[i][3];
    bool near_record = false;
    for (const double* p : recorded) {
      const double dx = world_eye[0] - p[0], dy = world_eye[1] - p[1], dz = world_eye[2] - p[2];
      if (dx * dx + dy * dy + dz * dz <= 0.005 * 0.005) {
        near_record = true;
        break;
      }
    }
    if (!near_record) continue;
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
  // D173: DiligentCore's pipeline cache let go once the stored pipelines are compiled and nothing is left unsaved: it
  // holds the driver's binaries of every stored pipeline (80 MB for Battlefield 1943's 2,200) for the whole run, while
  // the drivers keep caches of their own on disk (a start with no cache at all: the same 0.6 s). It is still read at
  // the next start, for a driver without one. Compilations in flight hold their own reference to it.
  if (pipeline_cache_ && !REXCVAR_GET(native_x_renderer_keep_pipeline_cache) && stored_.empty() &&
      stored_compiling_.empty() && !pipelines_unsaved_ && !saving_) {
    if (save_thread_.joinable()) save_thread_.join();
    pipelines_->SetCache(nullptr);
    pipeline_cache_.Release();
    REXLOG_INFO("[native_x_renderer] pipeline cache: let go after the stored pipelines (D173); the pipelines new in "
                "play compile without it");
  }
  if (primitives_) primitives_->EndFrame();
  if (memory_) memory_->EndFrame();
  if (guest_textures_) guest_textures_->BeginFrame(device_->frames_presented() + 1, device_->frames_completed());
  if (textures_) textures_->BeginFrame();
  // Object motion (D48): what DLAA did not draw (no main view this frame) is dropped.
  object_motion_.EndFrame();
  motion_draws_.clear();
  motion_data_.clear();
  skin_draws_.clear();  // (not drawn again this frame: the skin's overlay did not run)
  skin_data_.clear();
  glass_draws_.clear();  // D191: not drawn into a mask this frame
  glass_data_.clear();
  sea_draws_.clear();
  sea_data_.clear();
  sea_after_due_ = false;
  sea_after_source_.Release();
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

// The game's shadow map's view (ShadowMap): outdoorLightShadowmapTexture is the pixel shader's texture 0 in every
// shader that has it (tools/light_constants.py's tables).
void DrawPath::NoteShadowMap(uint32_t stage, uint32_t fetch_constant, IDeviceObject* view) {
  if (!shadow_map_pending_ || stage != 1 || fetch_constant != 0 || !view) return;
  shadow_map_pending_ = false;
  shadow_map_.view = static_cast<ITextureView*>(view);
  shadow_map_.frame = device_->frames_presented();
  static int said = 0;
  if (said < 2) {
    ++said;
    const TextureDesc& d = shadow_map_.view->GetTexture()->GetDesc();
    const TextureViewDesc& v = shadow_map_.view->GetDesc();
    REXLOG_INFO("[native_x_renderer] shadow map: texture type {} {}x{}x{} format {}, view dimension {} slices {} from {}; "
                "cascade 0 rows {:.5f} {:.5f} {:.5f} {:.3f} | {:.5f} {:.5f} {:.5f} {:.3f} | {:.5f} {:.5f} {:.5f} {:.3f}",
                int(d.Type), d.Width, d.Height, d.ArraySize, int(d.Format), int(v.TextureDim), v.NumArraySlices,
                v.FirstArraySlice, shadow_map_.cascades[0][0][0], shadow_map_.cascades[0][0][1],
                shadow_map_.cascades[0][0][2], shadow_map_.cascades[0][0][3], shadow_map_.cascades[0][1][0],
                shadow_map_.cascades[0][1][1], shadow_map_.cascades[0][1][2], shadow_map_.cascades[0][1][3],
                shadow_map_.cascades[0][2][0], shadow_map_.cascades[0][2][1], shadow_map_.cascades[0][2][2],
                shadow_map_.cascades[0][2][3]);
  }
}

// The wet copy of a texture of the first person weapon's (draw.h), or the texture itself: only in a weapon's draw,
// only its colour texture (or the developer's tinted one), only a DXT1 texture of 512 texels or more (a draw taken
// for the weapon's by mistake has other things there).
void DrawPath::SetSeaSky(const float zenith[3], const float horizon[3]) {
  for (int i = 0; i < 3; ++i) {
    if (std::abs(zenith[i] - sea_sky_[0][i]) > 0.004f || std::abs(horizon[i] - sea_sky_[1][i]) > 0.004f) sea_sky_dirty_ = true;
    sea_sky_[0][i] = zenith[i], sea_sky_[1][i] = horizon[i];
  }
}

// D190: Wake Island's and Coral Sea's sea shaders also take a day sky's cube map of their own (sampler_Texture19, fetch
// 6: the same picture as the window glass's, D182), blended into the water's colour, strongest at a grazing look; the
// game's blue sky stayed in the sea under any sky (the user, 17 Nov 2026: "isn't taking into account our sky, which
// makes it look much more blue like it's reflecting the original skybox"). While the renderer's own reflection is on,
// that cube is a small one of the renderer's sky: its zenith above, its horizon around (both as the view shows them,
// greyed by the clouds' cover), darker below.
IDeviceObject* DrawPath::SeaSkyView(uint32_t stage, uint32_t fetch_constant, IDeviceObject* view,
                                    const rg::SpirvShader& shader, bool cube) {
  // D240: with or without the renderer's own reflection, every cube map the sea's shaders take (the day sky's
  // outdoorLightReflectionEnvmap of all the maps' seas too) is this one, so the water takes the hour's sky.
  if (stage != 1 || (fetch_constant != 6 && !cube) || !own_sea_sky_ || !view || !IsSeaShader(shader.ucode_data_hash()) ||
      REXCVAR_GET(native_x_renderer_sea_game_light))  // D245: the game's own painted sky
    return view;
  constexpr uint32_t n = 4;
  if (!sea_sky_cube_) {
    TextureDesc d;
    d.Name = "NativeXRenderer the sea's sky cube";
    d.Type = RESOURCE_DIM_TEX_CUBE;
    d.Width = d.Height = n;
    d.ArraySize = 6;
    d.MipLevels = 1;
    d.Format = TEX_FORMAT_RGBA32_FLOAT;
    d.BindFlags = BIND_SHADER_RESOURCE;
    device_->device()->CreateTexture(d, nullptr, &sea_sky_cube_);
    if (!sea_sky_cube_) return view;
    sea_sky_dirty_ = true;
  }
  if (sea_sky_dirty_) {
    sea_sky_dirty_ = false;
    std::array<std::vector<float>, 6> faces;
    const float* z = sea_sky_[0];
    const float* h = sea_sky_[1];
    for (uint32_t f = 0; f < 6; ++f) {
      faces[f].resize(n * n * 4);
      for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
          // +Y (2) up, -Y (3) down; the sides from the zenith's way at their top to under the horizon at their foot.
          const float up = f == 2 ? 1.0f : f == 3 ? -1.0f : 0.75f - 0.5f * (float(y) + 0.5f) / float(n) * 2.0f;
          const float t = std::clamp(up, 0.0f, 1.0f), below = up < 0.0f ? 1.0f + 0.4f * up : 1.0f;
          float* p = &faces[f][(y * n + x) * 4];
          for (int c = 0; c < 3; ++c) p[c] = (h[c] + (z[c] - h[c]) * std::sqrt(t)) * below;
          p[3] = 1.0f;
        }
    }
    device_->recorder().Do([tex = sea_sky_cube_, faces = std::move(faces)](IDeviceContext* context) {
      for (uint32_t f = 0; f < 6; ++f) {
        TextureSubResData data{faces[f].data(), n * 4 * sizeof(float)};
        Box box{0, n, 0, n};
        context->UpdateTexture(tex, 0, f, box, data, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      }
    });
    static bool said = false;
    if (!said) {
      said = true;
      REXLOG_INFO("[native_x_renderer] sea reflection: the sea's own sky cube is the renderer's sky now (zenith {:.2f} "
                  "{:.2f} {:.2f}, horizon {:.2f} {:.2f} {:.2f})", z[0], z[1], z[2], h[0], h[1], h[2]);
    }
  }
  return sea_sky_cube_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

// The player's own weapon skin (weapon_skin.h): in a first person weapon's draw (not a vehicle's, not the hands'),
// its colour texture drawn with the skinned copy. The hands' material is told as WetView tells it.
IDeviceObject* DrawPath::SkinView(uint32_t stage, uint32_t fetch_constant, IDeviceObject* view,
                                  const rg::SpirvShader& shader, const rg::RegisterFile& regs) {
  if (stage != 1 || !weapon_draw_ || vehicle_draw_ || !view || !WeaponSkinNow()) return view;
  uint32_t squares = 0;
  bool hands = false;
  // Its colour texture: the largest DXT1 one (the guns' shaders also have a DXT5 map as large, their surface's relief
  // and shine, which skinned turned the Type 100 red and patchy: run skinB2), the highest numbered of equals.
  int colour = -1;
  uint32_t colour_width = 0;
  for (const auto& b : shader.GetTextureBindingsAfterTranslation()) {
    const auto f = regs.GetTextureFetch(b.fetch_constant);
    if (f.size_2d.width + 1 == 512 && f.size_2d.height + 1 == 256) hands = true;
    if (f.format == xenos::TextureFormat::k_DXT1 && f.size_2d.width + 1 == 512 && f.size_2d.height + 1 == 512) ++squares;
    if (f.format == xenos::TextureFormat::k_DXT1 && f.size_2d.width + 1 >= 256 &&
        (f.size_2d.width + 1 > colour_width || (f.size_2d.width + 1 == colour_width && int(b.fetch_constant) > colour))) {
      colour = int(b.fetch_constant), colour_width = f.size_2d.width + 1;
    }
  }
  {  // each pixel shader of the weapon's draws once: its textures, and whether it was taken for the hands'
    static std::set<uint64_t> said;
    if (said.size() < 24 && said.insert(shader.ucode_data_hash()).second) {
      std::string list;
      for (const auto& b : shader.GetTextureBindingsAfterTranslation()) {
        const auto f = regs.GetTextureFetch(b.fetch_constant);
        list += fmt::format(" {}:{}x{}/{}", b.fetch_constant, f.size_2d.width + 1, f.size_2d.height + 1, int(f.format));
      }
      REXLOG_INFO("[native_x_renderer] weapon skin: a weapon draw's pixel shader {:016X} ({} vertex constants{}), textures{}",
                  shader.ucode_data_hash(), weapon_constants_, hands || squares >= 3 ? ", the hands'" : "", list);
    }
  }
  if (int(fetch_constant) != colour) return view;
  if (!(hands || squares >= 3)) skin_gun_address_ = uint32_t(regs.GetTextureFetch(uint32_t(colour)).base_address);
  IDeviceObject* skinned = WeaponSkinView(view, hands || squares >= 3, device_->frames_presented());
  if (skinned != view) skin_tex_now_ = skinned;  // (the mesh pass draws this draw again, KeepSkinDraw)
  return skinned;
}

IDeviceObject* DrawPath::WetView(uint32_t stage, uint32_t fetch_constant, IDeviceObject* view,
                                 const rg::SpirvShader& shader, const rg::RegisterFile& regs) {
  if (stage != 1 || !(weapon_draw_ || vehicle_draw_) || !view || !seen_camera_valid_) return view;  // (D217: vehicles too)
  vehicle_relief_ = false;
  const bool tint = wet_tint_ >= 0;
  // Which texture: not by its number (the weapon's shaders differ: in some texture 2 is a small tiled detail, run
  // chk5) but by what it is: the one of 1024 texels or more, mapped once over the model. (The developer's check with
  // a number under 90 takes that texture whatever it is.)
  const bool by_number = tint && wet_tint_ < 90;
  if (by_number && fetch_constant != uint32_t(wet_tint_)) return view;
  if (!tint && wet_count_ * wet_show_ < 0.01f) return view;
  auto* original = static_cast<ITextureView*>(view);
  const TextureDesc& d = original->GetTexture()->GetDesc();
  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("[native_x_renderer] wetness: the weapon's texture {}: type {} {}x{}x{} format {}", fetch_constant,
                int(d.Type), d.Width, d.Height, d.ArraySize, int(d.Format));
  }
  if ((d.Type != RESOURCE_DIM_TEX_2D && !(d.Type == RESOURCE_DIM_TEX_2D_ARRAY && d.ArraySize == 1)) ||
      d.Width < (by_number ? 32u : 256u) || d.Height < (by_number ? 32u : 256u) ||
      (d.Format != TEX_FORMAT_BC1_UNORM && d.Format != TEX_FORMAT_BC3_UNORM && !IsWeaponSkinCopy(view))) {
    return view;
  }
  if (d.Format == TEX_FORMAT_RGBA16_FLOAT) {  // the player's weapon skin's copy: the wetness drawn over it
    static bool said_skin = false;
    if (!said_skin) {
      said_skin = true;
      REXLOG_INFO("[native_x_renderer] wetness: over the weapon skin's copy (texture {}, {}x{})", fetch_constant, d.Width, d.Height);
    }
  }
  // The hands' draws, whose texture gets no drops: their material has a 512 x 256 texture among its others, the
  // weapons' have none (runs wet1 and wlog2). (Not by the shader: a hosted match draws them with others, run mrg3.)
  const uint64_t pixel_shader = shader.ucode_data_hash();
  bool skin_shader = false;
  // (In a hosted match their material is another: there it has four 512 x 512 DXT1 textures, its colour among them,
  // where the weapon's has one: runs wlog3, slot1 to slot4 and bump2.)
  uint32_t squares = 0;
  for (const auto& b : shader.GetTextureBindingsAfterTranslation()) {
    const auto f = regs.GetTextureFetch(b.fetch_constant);
    if (f.size_2d.width + 1 == 512 && f.size_2d.height + 1 == 256) skin_shader = true;
    if (f.format == xenos::TextureFormat::k_DXT1 && f.size_2d.width + 1 == 512 && f.size_2d.height + 1 == 512) ++squares;
  }
  if (squares >= 3) skin_shader = true;
  if (vehicle_draw_) {
    skin_shader = false;  // D217: a vehicle's textures get drops
    // Only its colour: the highest numbered of its DXT1 textures of 1024 texels or more (the Sherman's shader
    // 63E723B912B04D74: texture 4, its colour; texture 1, as large, another map, which drawn wet left the tank nearly
    // black: runs vwet1, vtint1 and vtint4).
    int colour = -1;
    for (const auto& b : shader.GetTextureBindingsAfterTranslation()) {
      const auto f = regs.GetTextureFetch(b.fetch_constant);
      if (f.format == xenos::TextureFormat::k_DXT1 && f.size_2d.width + 1 >= 1024) colour = std::max(colour, int(b.fetch_constant));
    }
    // Its other large textures only for their relief (D217, the user: "Is the wetness not bump shaded anymore?"): the
    // drop shader raises drops in one it finds a normal map and leaves any other as it is.
    if (int(fetch_constant) != colour) {
      const auto f = regs.GetTextureFetch(fetch_constant);
      if (f.size_2d.width + 1 < 512) return view;
      vehicle_relief_ = true;
    }
  }
  {
    static std::set<uint64_t> said_shaders;
    if (said_shaders.size() < 16 && said_shaders.insert(pixel_shader ^ (uint64_t(fetch_constant) << 60)).second)
      REXLOG_INFO("[native_x_renderer] wetness: the weapon's pixel shader {:016X} texture {} ({}x{}){}; its vertex "
                  "shader takes {} constants", pixel_shader, fetch_constant, d.Width, d.Height, skin_shader ? ", skin" : "",
                  weapon_constants_);
  }
  for (WetCopy& w : wet_copies_) {
    if (w.original != original) continue;
    w.used = device_->frames_presented();
    if (skin_shader && !w.skin) w.skin = true, w.asked = true;  // a texture any skin shader draws with is skin's
    if (!w.asked && (!w.copy || std::abs(w.count - wet_count_) > 0.02f || std::abs(w.show - wet_show_) > 0.03f ||
                     std::abs(wet_seconds_ - w.seconds) > 0.0165)) {  // (D217: 60 times a second, 15 before: "The wetness shaders should all animate at a smooth 60fps")
      w.asked = true;
    }
    static int used = 0;
    if (w.copy && used < 2) {
      ++used;
      REXLOG_INFO("[native_x_renderer] wetness: a wet copy drawn with (texture {})", fetch_constant);
    }
    return w.copy ? static_cast<IDeviceObject*>(w.copy.RawPtr()) : view;
  }
  // At the most 64 textures have a copy; one not drawn with for five seconds is let go (TakeWetRequests). (At first
  // twelve, and when a thirteenth came all were dropped and made again: with a kit's several weapons that was every
  // frame, and the graphics memory ran out: run covJ.)
  if (wet_copies_.size() >= 64) return view;
  WetCopy w;
  w.original = original;
  w.vehicle = vehicle_draw_;  // D217
  w.relief_only = vehicle_draw_ && vehicle_relief_;
  w.used = device_->frames_presented();
  // The hands' skin shaders (Battlefield 1943: runs wet1 and wlog2), whose texture gets no drops.
  w.skin = skin_shader;
  w.asked = true;
  wet_copies_.push_back(std::move(w));
  return view;
}
bool DrawPath::TakeWetRequests(std::vector<WetRequest>& out) {
  out.clear();
  const uint64_t frame = device_->frames_presented();
  std::erase_if(wet_copies_, [frame](const WetCopy& w) { return frame > w.used + 300; });
  for (WetCopy& w : wet_copies_) {
    if (!w.asked) continue;
    w.asked = false;
    out.push_back({w.original, w.copy, w.skin, w.vehicle, w.relief_only});
  }
  return !out.empty();
}
void DrawPath::SetWetCopy(ITextureView* original, ITextureView* copy) {
  for (WetCopy& w : wet_copies_) {
    if (w.original != original) continue;
    w.copy = copy;
    w.count = wet_count_, w.show = wet_show_, w.seconds = wet_seconds_;
  }
}

bool DrawPath::WorldCamera(float eye[3], uint64_t* frame) const {
  if (!world_camera_valid_) return false;
  std::memcpy(eye, world_camera_, sizeof(world_camera_));
  if (frame) *frame = world_camera_frame_;
  return true;
}

bool DrawPath::DomeCamera(float m[4][4]) const {
  if (dome_frame_ != device_->frames_presented()) return false;
  std::memcpy(m, dome_m_, sizeof(dome_m_));
  return true;
}

uint64_t DrawPath::PixelShaderHash(const StateTracker::Program& program) {
  if (program.ucode.empty()) return 0;
  bool new_failure = false;
  rg::SpirvShader* shader = FindShader(xenos::ShaderType::kPixel, program, &new_failure);
  return shader ? shader->ucode_data_hash() : 0;
}

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
  // Research for puddles in craters ("have it create puddles during rainfall where craters form on the map",
  // 14 Nov 2026): does the terrain's height texture change when a crater forms? Once a second its data in the game's
  // memory is looked over in 256 parts, and a change is logged: how many parts, and how the heights there moved.
  {
    static uint64_t last_frame = 0;
    const uint64_t frame = device_->frames_presented();
    auto* kernel = rex::system::kernel_state();
    if (frame >= last_frame + 60 && kernel && kernel->memory()) {
      last_frame = frame;
      const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
      const size_t bytes = size_t(width) * height * 2;
      const uint8_t* data = kernel->memory()->TranslatePhysical<const uint8_t*>(uint32_t(fetch.base_address) << 12);
      static std::vector<uint64_t> seen;
      static uint32_t seen_base = 0;
      static int said = 0;
      if (data && bytes >= 4096 && bytes <= (64u << 20)) {
        const size_t part = bytes / 256;
        std::vector<uint64_t> now(256);
        for (size_t i = 0; i < 256; ++i) now[i] = XXH3_64bits(data + i * part, part);
        if (seen.size() == 256 && seen_base == uint32_t(fetch.base_address)) {
          uint32_t changed = 0, first = 256;
          for (uint32_t i = 0; i < 256; ++i)
            if (now[i] != seen[i]) ++changed, first = std::min(first, i);
          if (changed && said < 200) {
            ++said;
            REXLOG_INFO("[native_x_renderer] crater research: the terrain's heights changed in {} of 256 parts (the first: "
                        "part {}); {}x{} texels at {:05X}", changed, first, width, height, uint32_t(fetch.base_address));
          }
        } else {
          REXLOG_INFO("[native_x_renderer] crater research: watching the terrain's heights, {}x{} texels (format {}) at "
                      "{:05X}", width, height, uint32_t(fetch.format), uint32_t(fetch.base_address));
        }
        seen = std::move(now), seen_base = uint32_t(fetch.base_address);
      }
    }
  }
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
bool DrawPath::HudClipExtent(std::span<const Copy> copies, uint32_t count, double out[4]) const {
  float e[4];
  if (!InlineExtent(copies, count, e)) return false;
  const float cx[4] = {e[0], e[1], e[0], e[1]}, cy[4] = {e[2], e[2], e[3], e[3]};
  const size_t used = std::min(vertex_registers_.size(), vertex_constants_.size() / 16);
  auto reg = [&](size_t i) { return reinterpret_cast<const float*>(vertex_constants_.data() + i * 16); };
  auto place = [&](auto&& fx, auto&& fy) {
    double l = 1e30, h = -1e30, yl = 1e30, yh = -1e30;
    for (int k = 0; k < 4; ++k) {
      const double x = fx(cx[k], cy[k]), y = fy(cx[k], cy[k]);
      if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 6.0 || std::fabs(y) > 6.0) return false;
      l = std::min(l, x), h = std::max(h, x), yl = std::min(yl, y), yh = std::max(yh, y);
    }
    if (h - l < 1e-4 || yh - yl < 1e-4) return false;
    out[0] = l, out[1] = h, out[2] = yl, out[3] = yh;
    return true;
  };
  for (size_t i = 0; i + 1 < used; ++i) {
    if (vertex_registers_[i + 1] != vertex_registers_[i] + 1) continue;
    const float *a = reg(i), *b = reg(i + 1);
    if (place([&](float x, float y) { return double(a[0]) * x + a[1] * y + a[3]; },
              [&](float x, float y) { return double(b[0]) * x + b[1] * y + b[3]; }))
      return true;
  }
  for (size_t i = 0; i + 3 < used; ++i) {
    if (vertex_registers_[i + 3] != vertex_registers_[i] + 3) continue;
    const float *a = reg(i), *b = reg(i + 1), *d = reg(i + 3);
    if (place([&](float x, float y) { return double(a[0]) * x + b[0] * y + d[0]; },
              [&](float x, float y) { return double(a[1]) * x + b[1] * y + d[1]; }))
      return true;
  }
  return false;
}

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
  } else if (double marker_x = -1;
             !hud_menu_ &&
             // 13 Nov 2026 ("separate world markers ... so they don't interfere with player HUD elements"): what a
             // draw is drawn from tells them apart where its place cannot. The game's crosshairs, gunsights and scope
             // marks are on a sheet of their own (Battlefield 1943: menu/hud_crosshair_textures, 256 x 256, 8_8_8_8),
             // and nothing on it marks a place in the world: a draw from it is never a world marker, however near
             // one it passes.
             !(hud_sheet_format_ == 6 && hud_sheet_w_ == 256 && hud_sheet_h_ == 256) &&
             // Only a draw the size and shape of a marker's icon or of a letter of its label (6 to 44 canvas pixels
             // each way, not a line). The middle of the screen is where whatever the game projects as the aim moves
             // passes, and a sight drawn there in pieces was taken apart piece by piece as each came within reach of
             // such a point: a scope's crosshair and mil dots (13 Nov), then a plane's gunsight, whose ring is larger
             // than any marker and whose bars are lines ("invisible when you first get in the plane ... glitchy").
             [&] {
               const double w = (hi - lo) * 640.0, h = (y_hi - y_lo) * 360.0;
               // D206: a ring round a marker, square, up to 56 (the squad leader's order on a flag, 44 to 45: the
               // user, 19 Nov 2026, "instead staying locked over the flag icon, it drifts as the camera moves"; run
               // ring1). A plane's gunsight ring is far larger.
               const bool square = w <= h * 1.2 && h <= w * 1.2;
               static const bool old_limit = std::getenv("NXR_HUD_MARKER_44") != nullptr;  // (a diagnosis aid: as before)
               const double most = square && !old_limit ? 56.0 : 44.0;
               return w >= 6.0 && w <= most && h >= 6.0 && h <= most && w <= h * 2.5 && h <= w * 2.5;
             }() &&
             NearHudMarker(((lo + hi) * 0.5 + 1.0) * 640.0, (1.0 - (y_lo + y_hi) * 0.5) * 360.0, 48.0, &marker_x)) {
    // A world marker (the game's projection noted it, NoteHudMarker): its centre where the 3D view shows that place,
    // its size the HUD's. Every piece of one marker moves by the marker's own place, not by its own centre (13 Nov:
    // a plane's gunsight, drawn in several pieces around one projected point, came apart and shifted in flight).
    ++t.middle, ++t.markers, result = "world marker";
    place.mode = HudMode::kPoint;
    // Not through a scope: its crosshair and mil dots are drawn near the flags seen through it, and moved by a
    // flag's place they came off the sight as the camera turned (13 Nov: "displacing the crosshair and mildots").
    place.point_x = hud_scope_ ? -1 : marker_x;
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
    // The height it is scaled about (HudPlace::anchor_y), unless it took its place from a panel or the last draw.
    if (const std::string_view r = result; r != "in a panel" && r != "as the last" && r != "unknown, as the last") {
      const double yc = (y_lo + y_hi) * 0.5, xc = (lo + hi) * 0.5;
      place.anchor_y = yc > 0.34 ? 0.0f : yc < -0.34 ? 1.0f : 0.5f;
      place.anchor_x = xc < -0.34 ? 0.0f : xc > 0.34 ? 1.0f : 0.5f;
    }
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
        // NXR_HUD_MARKER_LOG=1 (a diagnosis aid, 19 Nov 2026: the squad leader's order ring drifts off its flag as the
        // camera turns): a draw that moves on at one size and is not taken for a world marker, once a size and sheet,
        // with its size in canvas pixels, its sheet and the nearest point the game projected.
        static const bool log = std::getenv("NXR_HUD_MARKER_LOG") != nullptr;
        if (log && std::string_view(result) != "world marker") {
          static std::set<uint64_t> said;
          const double w = (hi - lo) * 640.0, h = (y_hi - y_lo) * 360.0;
          const double px = ((lo + hi) * 0.5 + 1.0) * 640.0, py = (1.0 - (y_lo + y_hi) * 0.5) * 360.0;
          const uint64_t key = uint64_t(std::lround(w)) << 40 | uint64_t(std::lround(h)) << 24 |
                               uint64_t(hud_sheet_w_ & 0xFFF) << 12 | uint64_t(hud_sheet_h_ & 0xFFF);
          if (said.size() < 64 && said.insert(key).second) {
            double mx = -1;
            const bool close_by = NearHudMarker(px, py, 160.0, &mx);
            const bool in_reach = NearHudMarker(px, py, 48.0);
            REXLOG_INFO("[native_x_renderer] HUD marker log: a moving draw not taken for a world marker: {:.0f} x {:.0f} "
                        "canvas px at {:.0f}, {:.0f} ({}); sheet {}x{} format {}; vertex shader {:016X}; nearest "
                        "projected point x {} ({})",
                        w, h, px, py, result, hud_sheet_w_, hud_sheet_h_, hud_sheet_format_, vertex_shader,
                        close_by ? fmt::format("{:.0f}", mx) : std::string("none within 160 px"),
                        in_reach ? "within 48 px" : "not within 48 px");
          }
        }
      }
    }
    if (hud_seen_.size() < 4096) hud_seen_.push_back(seen);
  }
  // The scope research (Battlefield 1943 Reborn 0.4.1, the picture in picture scope): with the HUD debug on, each kind
  // of 2D draw once (its vertex shader, how it was placed, its extent), so the sniper scope's mask can be told apart.
  if (hud_debug_) {
    static std::set<std::pair<uint64_t, std::string>> seen_kinds;
    if (seen_kinds.size() < 400 && seen_kinds.insert({vertex_shader, result}).second)
      REXLOG_INFO("[native_x_renderer] HUD kind: vs {:016X} -> {} (quad {} x {:.2f}..{:.2f} y {:.2f}..{:.2f}, clip x {:.2f}..{:.2f} "
                  "y {:.2f}..{:.2f}, {} registers, phase {})",
                  vertex_shader, result, have_quad ? count : 0, cx[0], cx[1], cy[0], cy[2], lo, hi, y_lo, y_hi, used,
                  hud_menu_ ? "menu" : "game");
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
