// NativeXRenderer - the SDK's ImGui overlays on NativeXRenderer's swap chain (D75; see overlay.h).

#include "overlay.h"

#include "glsl.h"

#include <rex/logging.h>
#include <rex/ui/ui_system.h>

#include <ui_compat.h>  // the game's UI system (bf/src/compat)

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace Diligent;

namespace native_x_renderer {
namespace {

// Dear ImGui's vertices (ImDrawVert): position and texture coordinates in display coordinates, an RGBA8 color. Diligent's clip space has y up (D3D's convention), the coordinate space y down.
constexpr const char* kVertexShader = R"(#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(set = 0, binding = 0) uniform OverlayConstants {
  vec4 scale_offset;  // clip = position * xy + zw
};
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
  v_uv = in_uv;
  v_color = in_color;
  gl_Position = vec4(in_position * scale_offset.xy + scale_offset.zw, 0.0, 1.0);
}
)";

constexpr const char* kPixelShader = R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D overlay_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 color;
void main() {
  color = v_color * texture(overlay_texture, v_uv);
}
)";

RefCntAutoPtr<IShader> CreateShader(IRenderDevice* device, SHADER_TYPE type, const char* source, const char* name) {
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv =
      CompileGlsl(type == SHADER_TYPE_VERTEX ? GlslStage::kVertex : GlslStage::kPixel, source, name);
  if (spirv.empty()) return shader;
  ShaderCreateInfo info;
  info.Desc.Name = name;
  info.Desc.ShaderType = type;
  info.Desc.UseCombinedTextureSamplers = true;
  info.ByteCode = spirv.data();
  info.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  device->CreateShader(info, &shader);
  return shader;
}

// Grows a dynamic buffer to hold `bytes`.
bool Reserve(IRenderDevice* device, RefCntAutoPtr<IBuffer>& buffer, uint32_t& capacity, uint32_t bytes,
             Diligent::BIND_FLAGS bind, const char* name) {
  if (buffer && capacity >= bytes) return true;
  capacity = std::max<uint32_t>(bytes, std::max<uint32_t>(capacity * 2, 64 * 1024));
  BufferDesc desc;
  desc.Name = name;
  desc.Size = capacity;
  desc.Usage = USAGE_DYNAMIC;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = CPU_ACCESS_WRITE;
  buffer.Release();
  device->CreateBuffer(desc, nullptr, &buffer);
  return buffer != nullptr;
}

}  // namespace

bool Overlay::Initialize(IRenderDevice* device, TEXTURE_FORMAT target_format) {
  device_ = device;
  auto vs = CreateShader(device, SHADER_TYPE_VERTEX, kVertexShader, "NativeXRenderer overlay VS");
  auto ps = CreateShader(device, SHADER_TYPE_PIXEL, kPixelShader, "NativeXRenderer overlay PS");
  if (!vs || !ps) return false;

  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer overlay";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = target_format;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = False;
  auto& blend = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
  blend.BlendEnable = True;
  blend.SrcBlend = BLEND_FACTOR_SRC_ALPHA;
  blend.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
  blend.BlendOp = BLEND_OPERATION_ADD;
  blend.SrcBlendAlpha = BLEND_FACTOR_ONE;
  blend.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
  blend.BlendOpAlpha = BLEND_OPERATION_ADD;
  LayoutElement layout[] = {
      {0, 0, 2, VT_FLOAT32, False},
      {1, 0, 2, VT_FLOAT32, False},
      {2, 0, 4, VT_UINT8, True},
  };
  info.GraphicsPipeline.InputLayout.LayoutElements = layout;
  info.GraphicsPipeline.InputLayout.NumElements = 3;
  info.pVS = vs;
  info.pPS = ps;
  ShaderResourceVariableDesc variables[] = {
      {SHADER_TYPE_PIXEL, "overlay_texture", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
  };
  // Dear ImGui's textures (its font atlas, the achievement icons): filtered, clamped.
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "overlay_texture", linear}};
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
  info.PSODesc.ResourceLayout.Variables = variables;
  info.PSODesc.ResourceLayout.NumVariables = 1;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
  device->CreateGraphicsPipelineState(info, &pipeline_);
  if (!pipeline_) return false;

  BufferDesc constants;
  constants.Name = "NativeXRenderer overlay constants";
  constants.Size = 16;
  constants.Usage = USAGE_DYNAMIC;
  constants.BindFlags = BIND_UNIFORM_BUFFER;
  constants.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(constants, nullptr, &constants_);
  if (!constants_) return false;
  if (auto* variable = pipeline_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "OverlayConstants")) {
    variable->Set(constants_);
  }

  return true;
}

bool Overlay::MakeWhite() {
  if (white_) return true;
  const uint8_t rgba[4] = {255, 255, 255, 255};
  TextureDesc desc;
  desc.Name = "NativeXRenderer overlay white";
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = desc.Height = 1;
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureSubResData level;
  level.pData = rgba;
  level.Stride = 4;
  Diligent::TextureData init{&level, 1};
  auto entry = std::make_unique<TextureEntry>();
  device_->CreateTexture(desc, &init, &entry->texture);
  if (!entry->texture) return false;
  pipeline_->CreateShaderResourceBinding(&entry->srb, true);
  if (auto* variable = entry->srb->GetVariableByName(SHADER_TYPE_PIXEL, "overlay_texture")) {
    variable->Set(entry->texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  }
  white_ = std::move(entry);
  return true;
}

void Overlay::Present(IDeviceContext* context, ITextureView* target, uint32_t width, uint32_t height,
                      rex::ui::Window* window) {
  (void)window;
  if (!pipeline_ || !width || !height) return;
  rex::ui::IUISystem* ui = rex::ui::compat::GetUI();
  if (!ui) return;
  // RenderOverlays runs inside this call, on this thread, when a dialog is open.
  context_ = context;
  target_ = target;
  width_ = width;
  height_ = height;
  ui->DrawOverlays(*this);
  context_ = nullptr;
  target_ = nullptr;
}

// A texture made (or made again: an update replaces it whole, as the SDK's overlay does) from Dear ImGui's pixels.
bool Overlay::Upload(ImTextureData& texture) {
  const uint32_t width = uint32_t(texture.Width), height = uint32_t(texture.Height);
  if (!texture.Pixels || !width || !height) return false;
  const uint8_t* pixels = static_cast<const uint8_t*>(texture.GetPixels());
  std::vector<uint8_t> expanded;
  if (texture.Format == ImTextureFormat_Alpha8) {
    expanded.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
      expanded[i * 4 + 0] = expanded[i * 4 + 1] = expanded[i * 4 + 2] = 0xFF;
      expanded[i * 4 + 3] = pixels[i];
    }
    pixels = expanded.data();
  }
  TextureDesc desc;
  desc.Name = "NativeXRenderer overlay texture";
  desc.Type = RESOURCE_DIM_TEX_2D;
  desc.Width = width;
  desc.Height = height;
  desc.Format = TEX_FORMAT_RGBA8_UNORM;
  desc.Usage = USAGE_IMMUTABLE;
  desc.BindFlags = BIND_SHADER_RESOURCE;
  TextureSubResData level;
  level.pData = pixels;
  level.Stride = uint64_t(width) * 4;
  Diligent::TextureData init{&level, 1};
  auto entry = std::make_unique<TextureEntry>();
  device_->CreateTexture(desc, &init, &entry->texture);
  if (!entry->texture) return false;
  pipeline_->CreateShaderResourceBinding(&entry->srb, true);
  if (auto* variable = entry->srb->GetVariableByName(SHADER_TYPE_PIXEL, "overlay_texture")) {
    variable->Set(entry->texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
  }
  std::unique_ptr<TextureEntry>& slot = textures_[&texture];
  if (slot) retired_.push_back({frame_, std::move(slot)});
  slot = std::move(entry);
  texture.SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(slot.get())));
  texture.SetStatus(ImTextureStatus_OK);
  return true;
}

void Overlay::UpdateTextures(ImDrawData& draw_data) {
  ++frame_;
  constexpr uint64_t kRetireFrames = 4;
  std::erase_if(retired_, [this](const Retired& r) { return frame_ - r.frame >= kRetireFrames; });
  if (!draw_data.Textures) return;
  for (ImTextureData* texture : *draw_data.Textures) {
    switch (texture->Status) {
      case ImTextureStatus_WantCreate:
      case ImTextureStatus_WantUpdates:
        Upload(*texture);
        break;
      case ImTextureStatus_WantDestroy:
        if (auto found = textures_.find(texture); found != textures_.end()) {
          retired_.push_back({frame_, std::move(found->second)});
          textures_.erase(found);
        }
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
        break;
      default:
        break;
    }
  }
}

void Overlay::ReleaseTextures() {
  for (auto& [texture, entry] : textures_) {
    auto* data = const_cast<ImTextureData*>(texture);
    data->SetTexID(ImTextureID_Invalid);
    data->SetStatus(ImTextureStatus_WantCreate);
  }
  textures_.clear();
  retired_.clear();
}

void Overlay::RenderOverlays(ImDrawData& draw_data) {
  IDeviceContext* context = context_;
  ITextureView* target = target_;
  if (!context || !target) return;
  UpdateTextures(draw_data);
  const float display_w = draw_data.DisplaySize.x, display_h = draw_data.DisplaySize.y;
  if (draw_data.CmdListsCount == 0 || !(display_w > 0.0f) || !(display_h > 0.0f)) return;
  if (draw_data.TotalVtxCount <= 0 || draw_data.TotalIdxCount <= 0 || !MakeWhite()) return;
  uint32_t vertex_bytes = uint32_t(draw_data.TotalVtxCount) * sizeof(ImDrawVert);
  uint32_t index_bytes = uint32_t(draw_data.TotalIdxCount) * sizeof(ImDrawIdx);
  if (!Reserve(device_, vertices_, vertex_capacity_, vertex_bytes, Diligent::BIND_VERTEX_BUFFER,
               "NativeXRenderer overlay vertices") ||
      !Reserve(device_, indices_, index_capacity_, index_bytes, Diligent::BIND_INDEX_BUFFER,
               "NativeXRenderer overlay indices")) {
    return;
  }
  // All the lists in one vertex and one index buffer, each list's draws offset by where its data starts.
  void* data = nullptr;
  context->MapBuffer(vertices_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  auto* vertex_out = static_cast<uint8_t*>(data);
  for (const ImDrawList* list : draw_data.CmdLists) {
    size_t bytes = size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
    std::memcpy(vertex_out, list->VtxBuffer.Data, bytes);
    vertex_out += bytes;
  }
  context->UnmapBuffer(vertices_, MAP_WRITE);
  context->MapBuffer(indices_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  auto* index_out = static_cast<uint8_t*>(data);
  for (const ImDrawList* list : draw_data.CmdLists) {
    size_t bytes = size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    std::memcpy(index_out, list->IdxBuffer.Data, bytes);
    index_out += bytes;
  }
  context->UnmapBuffer(indices_, MAP_WRITE);
  // Display coordinates (DisplayPos at the top left, y down) to Diligent's clip space (y up).
  context->MapBuffer(constants_, MAP_WRITE, MAP_FLAG_DISCARD, data);
  float scale_offset[4] = {2.0f / display_w, -2.0f / display_h, -1.0f - draw_data.DisplayPos.x * 2.0f / display_w,
                           1.0f + draw_data.DisplayPos.y * 2.0f / display_h};
  std::memcpy(data, scale_offset, sizeof(scale_offset));
  context->UnmapBuffer(constants_, MAP_WRITE);

  const uint32_t width = width_, height = height_;
  context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->SetPipelineState(pipeline_);
  Viewport viewport;
  viewport.Width = float(width);
  viewport.Height = float(height);
  context->SetViewports(1, &viewport, 0, 0);
  IBuffer* buffers[] = {vertices_};
  uint64_t offsets[] = {0};
  context->SetVertexBuffers(0, 1, buffers, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            SET_VERTEX_BUFFERS_FLAG_RESET);
  context->SetIndexBuffer(indices_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  // The clip rectangles, from display coordinates to the target's pixels.
  const float scale_x = float(width) / display_w, scale_y = float(height) / display_h;
  auto clamp = [](float v, uint32_t limit) {
    return int32_t(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, float(limit)));
  };
  TextureEntry* bound = nullptr;
  uint32_t list_vertex_base = 0, list_index_base = 0;
  for (const ImDrawList* list : draw_data.CmdLists) {
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback || !cmd.ElemCount) continue;
      const ImTextureID id = cmd.GetTexID();
      TextureEntry* texture =
          id == ImTextureID_Invalid ? white_.get() : reinterpret_cast<TextureEntry*>(static_cast<std::uintptr_t>(id));
      Rect scissor;
      scissor.left = clamp(std::floor((cmd.ClipRect.x - draw_data.DisplayPos.x) * scale_x), width);
      scissor.top = clamp(std::floor((cmd.ClipRect.y - draw_data.DisplayPos.y) * scale_y), height);
      scissor.right = clamp(std::ceil((cmd.ClipRect.z - draw_data.DisplayPos.x) * scale_x), width);
      scissor.bottom = clamp(std::ceil((cmd.ClipRect.w - draw_data.DisplayPos.y) * scale_y), height);
      if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
      context->SetScissorRects(1, &scissor, 0, 0);
      if (texture != bound) {
        context->CommitShaderResources(texture->srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        bound = texture;
      }
      DrawIndexedAttribs attribs;
      attribs.NumIndices = cmd.ElemCount;
      attribs.IndexType = sizeof(ImDrawIdx) == 2 ? VT_UINT16 : VT_UINT32;
      attribs.FirstIndexLocation = list_index_base + cmd.IdxOffset;
      attribs.BaseVertex = list_vertex_base + cmd.VtxOffset;
      context->DrawIndexed(attribs);
    }
    list_vertex_base += uint32_t(list->VtxBuffer.Size);
    list_index_base += uint32_t(list->IdxBuffer.Size);
  }
}

}  // namespace native_x_renderer
