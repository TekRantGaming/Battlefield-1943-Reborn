// NativeXRenderer - weapon skins on the player's own first person weapon (weapon_skin.h).

#include "weapon_skin.h"

#include <native_x_renderer/native_x_renderer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>
#include <utility>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"
#include "RefCntAutoPtr.hpp"

#include <rex/logging.h>

#include "glsl.h"

using namespace Diligent;

namespace native_x_renderer {
namespace {

std::atomic<int> g_skin{0};
std::atomic<int64_t> g_skin_at{0};  // steady clock ticks

constexpr const char* kVS = R"(#version 450
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The electric skin (the reference picture TekRantGaming chose: a gun near black, glowing blue-white lightning
// branching over it, gold parts). In the texture's own space, so the arcs stay on the gun as it moves: lines where a
// warped noise crosses its middle value (the main arcs, and finer branches beside them), jittered a little 15 times a
// second (the crackle), crawling slowly, with a pulse running along them and patches flaring by turns.
constexpr const char* kPS = R"(#version 450
layout(std140, set = 0, binding = 0) uniform SkinConstants {
  vec4 k;  // x the clock (seconds), y and z the texture's size, w the arcs' brightness
  vec4 k2; // x the most any channel may be
};
layout(set = 0, binding = 1) uniform sampler2D srcTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float h21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(h21(i), h21(i + vec2(1.0, 0.0)), u.x), mix(h21(i + vec2(0.0, 1.0)), h21(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p) {
  float s = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) {
    s += a * vnoise(p);
    p = p * 2.03 + vec2(17.1, 9.2);
    a *= 0.5;
  }
  return s;
}
float fbm2(vec2 p) { return 0.667 * vnoise(p) + 0.333 * vnoise(p * 2.03 + vec2(17.1, 9.2)); }
// How near p is to an arc of this noise, in texels of the copy (fwidth: one texel's change of the noise), so an arc
// is as thin on every texture: x its bright core (about `core` texels across), y its glow (falling off over `glow`).
vec2 Arc(vec2 p, float core, float glow) {
  float n = fbm(p);
  float d = abs(n - 0.5) / max(fwidth(n), 1e-5);
  return vec2(1.0 - smoothstep(0.0, core, d), exp(-d / glow));
}

void main() {
  vec4 c = texture(srcTex, v_uv);
  float t = k.x;
  // The place in arcs: about six across a texture of 1024 texels, whatever its size.
  vec2 p = v_uv * k.yz / 1024.0 * 6.0;
  // Crawl, then the crackle: a fine jitter seeded anew 15 times a second.
  float tick = floor(t * 15.0);
  vec2 q = p + vec2(0.05 * t, -0.03 * t);
  q += 0.9 * vec2(fbm2(p * 0.7 + 3.1), fbm2(p * 0.7 + 8.7)) - 0.45;
  q += 0.03 * (vec2(vnoise(p * 24.0 + tick * 7.13), vnoise(p * 24.0 + tick * 3.71 + 19.0)) - 0.5);
  vec2 main_arc = Arc(q, 1.5, 10.0);
  vec2 branch = Arc(q * 2.3 + vec2(5.2, 1.7) + 0.02 * t, 1.0, 4.0);
  // Branches near the main arcs only, as forks off them.
  float near_main = smoothstep(0.15, 0.6, main_arc.y);
  float line = max(main_arc.x, branch.x * 0.6 * near_main);
  float halo = max(main_arc.y, branch.y * 0.3 * near_main);
  // A pulse running along them, and patches flaring by turns (each a few times a second).
  float pulse = 0.6 + 0.4 * sin(t * 4.0 - vnoise(p * 0.5) * 12.0);
  vec2 cell = floor(p * 0.6);
  float flare = 1.0 + 0.6 * step(0.8, h21(cell + floor(t * 9.0 + h21(cell) * 9.0)));
  float energy = pulse * flare;

  // The surface (the reference: a black body, gold furniture): metal near black with a faint marbling, wood (warm)
  // gold, both keeping a little of the texture's own detail.
  float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
  float warmth = (c.r - c.b) / (max(c.r, max(c.g, c.b)) + 0.03);
  float wood = smoothstep(0.30, 0.45, warmth);
  vec3 black = (vec3(0.018, 0.022, 0.030) + c.rgb * 0.10) * (0.7 + 0.6 * vnoise(p * 1.7 + 40.0));
  vec3 gold = vec3(0.95, 0.70, 0.26) * (0.40 + 0.70 * lum);
  vec3 surface = mix(black, gold, wood);
  // The arcs: blue-white cores in a blue glow, brightest over the black, fainter over the gold.
  float over = mix(1.0, 0.45, wood);
  vec3 glow = vec3(0.38, 0.80, 1.0) * line * k.w + vec3(0.0, 0.45, 1.0) * halo * halo * 0.6 * k.w;
  // (Each channel at most k2.x: brighter overflows the game's colour target, see WeaponSkinMake.)
  out_color = vec4(min(surface + glow * energy * over, vec3(k2.x)), c.a);
}
)";

// The arcs' bloom (TekRantGaming, 21 Nov 2026: "a very subtle bloom effect that's got a blueish hue, it's meant to
// look like lightning strikes"): the view's picture a quarter its size, only its arcs kept (bright, far more blue than
// red, nearer than 3 m: the first person weapon's), their mips spreading them, laid over the picture in blue.
constexpr const char* kBloomCommon = R"(#version 450
layout(std140, set = 0, binding = 0) uniform BloomConstants {
  vec4 a;  // xy the view's size (its part of the colour), zw the depth's part
  vec4 b;  // xy the quarter texture's size, z the strength, w a developer's view (1: the bloom alone)
};
)";
constexpr const char* kBloomExtractPS = R"(
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(location = 0) out vec4 out_color;
void main() {
  vec2 at = floor(gl_FragCoord.xy) * 4.0;  // this quarter pixel's first view pixel
  vec3 sum = vec3(0.0);
  float kept = 0.0;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      vec2 p = at + vec2(x, y) * 2.0 + 1.0;
      ivec2 cp = ivec2(min(p, a.xy - 1.0));
      ivec2 dp = ivec2(min(p * a.zw / a.xy, a.zw - 1.0));
      vec3 c = max(texelFetch(colorTex, cp, 0).rgb, vec3(0.0));
      float depth = texelFetch(depthTex, dp, 0).r;  // the game's: 0.1 / distance
      float near = depth > 0.0333 ? 1.0 : 0.0;       // nearer than 3 m: the hands and the gun
      float blue = (c.b - max(c.r, c.g * 0.7)) / (c.b + 0.02);
      float w = near * smoothstep(0.25, 0.55, blue) * smoothstep(0.04, 0.3, c.b);
      sum += c * w;
      kept += w;
    }
  out_color = vec4(sum * 0.25, kept * 0.25);
}
)";
constexpr const char* kBloomApplyPS = R"(
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(location = 0) out vec4 out_color;
void main() {
  vec2 uv = gl_FragCoord.xy * 0.25 / b.xy;
  vec3 m = textureLod(bloomTex, uv, 1.0).rgb * 0.45 + textureLod(bloomTex, uv, 2.0).rgb * 0.35 +
           textureLod(bloomTex, uv, 3.0).rgb * 0.20;
  // Round the arcs, not on them (the arcs themselves are as bright as the picture can hold): less where an arc is.
  vec3 core = textureLod(bloomTex, uv, 0.0).rgb;
  // Blue whatever the arcs' own colour came out as: their brightness, tinted.
  float glow = max(dot(m - core * 0.8, vec3(0.2, 0.3, 0.5)), 0.0);
  out_color = vec4(vec3(0.30, 0.58, 1.0) * glow * b.z, 1.0);
}
)";

RefCntAutoPtr<IShader> Compile(IRenderDevice* device, SHADER_TYPE type, const char* source, const char* name) {
  RefCntAutoPtr<IShader> shader;
  std::vector<uint32_t> spirv = CompileGlsl(type == SHADER_TYPE_VERTEX ? GlslStage::kVertex : GlslStage::kPixel, source, name);
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

struct Copy {
  RefCntAutoPtr<ITextureView> original, copy;
  uint64_t used = 0;     // the frame it was last drawn with
  double made = -1.0;    // the clock when the copy was last made
  int skin = 0;          // the skin it was made with
};

struct State {
  std::vector<Copy> copies;
  RefCntAutoPtr<IPipelineState> pso;
  RefCntAutoPtr<IShaderResourceBinding> srb;
  RefCntAutoPtr<IBuffer> constants;
  bool failed = false;
  uint64_t made_frame = ~0ull;
};
State& S() {
  static State s;
  return s;
}

double Clock() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool Ready(IRenderDevice* device) {
  State& s = S();
  if (s.srb) return true;
  if (s.failed) return false;
  s.failed = true;
  RefCntAutoPtr<IShader> vs = Compile(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer weapon skin VS");
  RefCntAutoPtr<IShader> ps = Compile(device, SHADER_TYPE_PIXEL, kPS, "NativeXRenderer weapon skin PS");
  if (!vs || !ps) {
    REXLOG_ERROR("[native_x_renderer] weapon skin: its shaders did not compile; the skin is not drawn");
    return false;
  }
  BufferDesc b;
  b.Name = "NativeXRenderer weapon skin constants";
  b.Size = 32;
  b.Usage = USAGE_DYNAMIC;
  b.BindFlags = BIND_UNIFORM_BUFFER;
  b.CPUAccessFlags = CPU_ACCESS_WRITE;
  device->CreateBuffer(b, nullptr, &s.constants);
  SamplerDesc linear;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "srcTex", linear}};
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = "NativeXRenderer weapon skin";
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = TEX_FORMAT_RGBA16_FLOAT;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  info.pVS = vs;
  info.pPS = ps;
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
  info.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
  device->CreateGraphicsPipelineState(info, &s.pso);
  if (s.pso) s.pso->CreateShaderResourceBinding(&s.srb, true);
  if (!s.srb || !s.constants) {
    REXLOG_ERROR("[native_x_renderer] weapon skin: its pipeline could not be made; the skin is not drawn");
    s.srb.Release();
    return false;
  }
  s.failed = false;
  return true;
}

bool MakeOne(IRenderDevice* device, IDeviceContext* context, Copy& c, double now, float bright, float most) {
  State& s = S();
  if (!c.copy) {
    TextureDesc d = c.original->GetTexture()->GetDesc();
    d.Name = "NativeXRenderer weapon skin copy";
    d.Format = TEX_FORMAT_RGBA16_FLOAT;
    d.MipLevels = 0;  // all of them
    d.Usage = USAGE_DEFAULT;
    d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    d.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
    d.CPUAccessFlags = CPU_ACCESS_NONE;
    RefCntAutoPtr<ITexture> texture;
    device->CreateTexture(d, nullptr, &texture);
    if (!texture) return false;
    c.copy = texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    REXLOG_INFO("[native_x_renderer] weapon skin: a skinned copy of the weapon's {}x{} colour texture", d.Width, d.Height);
  }
  ITexture* to = c.copy->GetTexture();
  const TextureDesc& desc = to->GetDesc();
  // Read as a plain 2D texture (the game's are arrays of one layer); written as the copy is.
  TextureViewDesc plain;
  plain.ViewType = TEXTURE_VIEW_SHADER_RESOURCE;
  plain.TextureDim = RESOURCE_DIM_TEX_2D;
  RefCntAutoPtr<ITextureView> from;
  c.original->GetTexture()->CreateView(plain, &from);
  TextureViewDesc first;
  first.ViewType = TEXTURE_VIEW_RENDER_TARGET;
  first.TextureDim = desc.Type;
  first.MostDetailedMip = 0, first.NumMipLevels = 1;
  first.FirstArraySlice = 0, first.NumArraySlices = 1;
  RefCntAutoPtr<ITextureView> target;
  to->CreateView(first, &target);
  if (!from || !target) return false;
  {
    const float k[8] = {float(std::fmod(now, 3600.0)), float(desc.Width), float(desc.Height), bright, most, 0, 0, 0};
    void* data = nullptr;
    context->MapBuffer(s.constants, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, k, sizeof(k));
    context->UnmapBuffer(s.constants, MAP_WRITE);
  }
  if (auto* v = s.srb->GetVariableByName(SHADER_TYPE_PIXEL, "SkinConstants")) v->Set(s.constants);
  if (auto* v = s.srb->GetVariableByName(SHADER_TYPE_PIXEL, "srcTex")) v->Set(from);
  ITextureView* rtv = target;
  context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Viewport viewport;
  viewport.Width = float(desc.Width);
  viewport.Height = float(desc.Height);
  viewport.MaxDepth = 1.0f;
  context->SetViewports(1, &viewport, 0, 0);
  context->SetPipelineState(s.pso);
  context->CommitShaderResources(s.srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  DrawAttribs attribs;
  attribs.NumVertices = 3;
  context->Draw(attribs);
  context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  context->GenerateMips(c.copy);
  c.made = now;
  return true;
}

}  // namespace

void SetWeaponSkin(int skin) {
  g_skin.store(skin, std::memory_order_relaxed);
  g_skin_at.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

int WeaponSkinNow() {
  const int skin = g_skin.load(std::memory_order_relaxed);
  if (!skin) return 0;
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  const int64_t quarter = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count();
  return now - g_skin_at.load(std::memory_order_relaxed) <= quarter ? skin : 0;
}

IDeviceObject* WeaponSkinView(IDeviceObject* view, bool hands, uint64_t frame) {
  if (!view || hands) return view;
  const int skin = WeaponSkinNow();
  if (!skin) return view;
  State& s = S();
  if (s.failed) return view;
  auto* original = static_cast<ITextureView*>(view);
  const TextureDesc& d = original->GetTexture()->GetDesc();
  if ((d.Type != RESOURCE_DIM_TEX_2D && !(d.Type == RESOURCE_DIM_TEX_2D_ARRAY && d.ArraySize == 1)) || d.Width < 256 ||
      d.Height < 256 || (d.Format != TEX_FORMAT_BC1_UNORM && d.Format != TEX_FORMAT_BC3_UNORM)) {
    return view;
  }
  for (Copy& c : s.copies) {
    if (c.original != original) continue;
    c.used = frame;
    return c.copy && c.skin == skin ? static_cast<IDeviceObject*>(c.copy.RawPtr()) : view;
  }
  if (s.copies.size() >= 8) return view;  // (a gun's few textures; a kit change lets the last ones go)
  Copy c;
  c.original = original;
  c.used = frame;
  s.copies.push_back(std::move(c));
  return view;
}

bool IsWeaponSkinCopy(IDeviceObject* view) {
  if (!view) return false;
  for (const Copy& c : S().copies)
    if (c.copy && static_cast<IDeviceObject*>(c.copy.RawPtr()) == view) return true;
  return false;
}

bool WeaponSkinMake(IRenderDevice* device, IDeviceContext* context, uint64_t frame, float wet) {
  State& s = S();
  if (s.copies.empty() || s.made_frame == frame) return false;
  s.made_frame = frame;
  std::erase_if(s.copies, [frame](const Copy& c) { return frame > c.used + 300; });
  const int skin = WeaponSkinNow();
  if (!skin || !device || !context || !Ready(device)) return false;
  const double now = Clock();
  // The arcs' brightness: twice white, the same by day and by night. More overflows what the game draws into: at
  // night from 4 the arcs' brightest places turned orange and red, from 8 in rainbow fringes (run skinN1, the
  // developer's check below stepping it 1 to 32); at 36, the first try at matching the night's darkness, the Type 100
  // was red all over (skinB2).
  float bright = 2.0f;
  // NXR_SKIN_BRIGHT=<a>,<b>,...: a developer's check of the arcs' brightness, each value in turn for 8 s.
  static const std::vector<float> steps = [] {
    std::vector<float> v;
    if (const char* e = std::getenv("NXR_SKIN_BRIGHT"))
      for (const char* c = e; *c;) {
        v.push_back(float(std::atof(c)));
        while (*c && *c != ',') ++c;
        if (*c) ++c;
      }
    return v;
  }();
  if (!steps.empty()) {
    static const double from = now;
    static int said = -1;
    const int i = int((now - from) / 8.0) % int(steps.size());
    bright = steps[size_t(i)];
    if (i != said) {
      said = i;
      REXLOG_INFO("[native_x_renderer] weapon skin: the arcs' brightness {:.1f} (NXR_SKIN_BRIGHT)", bright);
    }
  }
  // The most any channel may be: 2.5 dry, white (1) as soon as the gun is wet. Wet, the game's shader gives the gun
  // more shine, and the arcs' brightest places overflowed its colour target at night in orange and red outlines (run
  // skinH_nightUS capped at 2.5; skinI_nightUS at 2.5 to 1.3 by the wetness, red while it was still a little wet);
  // dry, peaks of 4.4 were clean (run skinN1). The bloom keeps their glow.
  const float w = std::clamp(wet / 0.08f, 0.0f, 1.0f);
  const float most = 2.5f - 1.5f * w * w * (3.0f - 2.0f * w);
  bool drew = false;
  for (Copy& c : s.copies) {
    if (frame > c.used + 2) continue;  // not drawn with now: made anew when it is
    if (c.copy && c.skin == skin && now - c.made < 1.0 / 30.0 - 0.001) continue;
    c.skin = skin;
    drew |= MakeOne(device, context, c, now, bright, most);
  }
  return drew;
}


namespace {
struct Bloom {
  RefCntAutoPtr<IShader> vs, extract_ps, apply_ps;
  RefCntAutoPtr<IPipelineState> extract, apply;
  RefCntAutoPtr<IShaderResourceBinding> extract_srb, apply_srb;
  RefCntAutoPtr<IBuffer> constants;
  RefCntAutoPtr<ITexture> quarter;
  TEXTURE_FORMAT apply_format = TEX_FORMAT_UNKNOWN;
  bool failed = false;
};
Bloom& B() {
  static Bloom b;
  return b;
}
RefCntAutoPtr<IPipelineState> BloomPipeline(IRenderDevice* device, const char* name, IShader* ps, TEXTURE_FORMAT format,
                                            bool add, std::initializer_list<std::pair<const char*, bool>> textures) {
  GraphicsPipelineStateCreateInfo info;
  info.PSODesc.Name = name;
  info.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
  info.GraphicsPipeline.NumRenderTargets = 1;
  info.GraphicsPipeline.RTVFormats[0] = format;
  info.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
  info.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  info.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
  info.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
  if (add) {  // added to the picture; its alpha kept
    RenderTargetBlendDesc& rt = info.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt.BlendEnable = True;
    rt.SrcBlend = BLEND_FACTOR_ONE;
    rt.DestBlend = BLEND_FACTOR_ONE;
    rt.BlendOp = BLEND_OPERATION_ADD;
    rt.SrcBlendAlpha = BLEND_FACTOR_ZERO;
    rt.DestBlendAlpha = BLEND_FACTOR_ONE;
    rt.BlendOpAlpha = BLEND_OPERATION_ADD;
    rt.RenderTargetWriteMask = COLOR_MASK_RGB;
  }
  info.pVS = B().vs;
  info.pPS = ps;
  SamplerDesc point, linear;
  point.MinFilter = point.MagFilter = point.MipFilter = FILTER_TYPE_POINT;
  point.AddressU = point.AddressV = point.AddressW = TEXTURE_ADDRESS_CLAMP;
  linear.MinFilter = linear.MagFilter = linear.MipFilter = FILTER_TYPE_LINEAR;
  linear.AddressU = linear.AddressV = linear.AddressW = TEXTURE_ADDRESS_CLAMP;
  std::vector<ImmutableSamplerDesc> samplers;
  for (const auto& [texture, lin] : textures) samplers.push_back({SHADER_TYPE_PIXEL, texture, lin ? linear : point});
  info.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  info.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
  info.PSODesc.ResourceLayout.NumImmutableSamplers = uint32_t(samplers.size());
  RefCntAutoPtr<IPipelineState> pso;
  device->CreateGraphicsPipelineState(info, &pso);
  return pso;
}
}  // namespace

bool WeaponSkinBloom(IRenderDevice* device, IDeviceContext* context, const WeaponSkinBloomInputs& in) {
  Bloom& b = B();
  if (b.failed || !device || !context || !in.color || !in.depth_srv || in.width < 64 || in.height < 64) return false;
  if (!b.extract) {
    b.failed = true;
    b.vs = Compile(device, SHADER_TYPE_VERTEX, kVS, "NativeXRenderer weapon skin bloom VS");
    b.extract_ps = Compile(device, SHADER_TYPE_PIXEL, (std::string(kBloomCommon) + kBloomExtractPS).c_str(),
                           "NativeXRenderer weapon skin bloom extract PS");
    b.apply_ps = Compile(device, SHADER_TYPE_PIXEL, (std::string(kBloomCommon) + kBloomApplyPS).c_str(),
                         "NativeXRenderer weapon skin bloom apply PS");
    BufferDesc d;
    d.Name = "NativeXRenderer weapon skin bloom constants";
    d.Size = 32;
    d.Usage = USAGE_DYNAMIC;
    d.BindFlags = BIND_UNIFORM_BUFFER;
    d.CPUAccessFlags = CPU_ACCESS_WRITE;
    device->CreateBuffer(d, nullptr, &b.constants);
    if (!b.vs || !b.extract_ps || !b.apply_ps || !b.constants) {
      REXLOG_ERROR("[native_x_renderer] weapon skin: the bloom's shaders did not compile; no bloom");
      return false;
    }
    b.extract = BloomPipeline(device, "NativeXRenderer weapon skin bloom extract", b.extract_ps, TEX_FORMAT_RGBA16_FLOAT,
                              false, {{"colorTex", false}, {"depthTex", false}});
    if (b.extract) b.extract->CreateShaderResourceBinding(&b.extract_srb, true);
    if (!b.extract_srb) {
      b.extract.Release();
      REXLOG_ERROR("[native_x_renderer] weapon skin: no bloom pipeline");
      return false;
    }
    b.failed = false;
  }
  const TEXTURE_FORMAT format = in.color->GetDesc().Format;
  if (!b.apply || b.apply_format != format) {
    b.apply.Release(), b.apply_srb.Release();
    b.apply = BloomPipeline(device, "NativeXRenderer weapon skin bloom apply", b.apply_ps, format, true, {{"bloomTex", true}});
    if (b.apply) b.apply->CreateShaderResourceBinding(&b.apply_srb, true);
    b.apply_format = format;
    if (!b.apply_srb) {
      REXLOG_ERROR("[native_x_renderer] weapon skin: no bloom pipeline for the format {}", int(format));
      return false;
    }
    REXLOG_INFO("[native_x_renderer] weapon skin: the arcs' bloom over a picture of format {}", int(format));
  }
  const uint32_t qw = (in.width + 3) / 4, qh = (in.height + 3) / 4;
  if (!b.quarter || b.quarter->GetDesc().Width != qw || b.quarter->GetDesc().Height != qh) {
    b.quarter.Release();
    TextureDesc d;
    d.Name = "NativeXRenderer weapon skin bloom";
    d.Type = RESOURCE_DIM_TEX_2D;
    d.Width = qw, d.Height = qh;
    d.MipLevels = 4;
    d.Format = TEX_FORMAT_RGBA16_FLOAT;
    d.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    d.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;
    device->CreateTexture(d, nullptr, &b.quarter);
    if (!b.quarter) return false;
  }
  {
    static const float show = [] {
      const char* v = std::getenv("NXR_SKIN_BLOOM_SHOW");
      return v && *v ? float(std::atof(v)) : 0.0f;
    }();
    static const float strength = [] {
      const char* v = std::getenv("NXR_SKIN_BLOOM");
      return v && *v ? float(std::atof(v)) : 0.5f;
    }();
    const float k[8] = {float(in.width), float(in.height), float(in.depth_width), float(in.depth_height),
                        float(qw), float(qh), strength, show};
    void* data = nullptr;
    context->MapBuffer(b.constants, MAP_WRITE, MAP_FLAG_DISCARD, data);
    std::memcpy(data, k, sizeof(k));
    context->UnmapBuffer(b.constants, MAP_WRITE);
  }
  ITextureView* color_srv = in.color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  ITextureView* color_rtv = in.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
  if (!color_srv || !color_rtv) return false;
  DrawAttribs draw;
  draw.NumVertices = 3;
  auto run = [&](IPipelineState* pso, IShaderResourceBinding* srb, ITextureView* rtv, uint32_t w, uint32_t h,
                 std::initializer_list<std::pair<const char*, IDeviceObject*>> inputs) {
    context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport viewport;
    viewport.Width = float(w);
    viewport.Height = float(h);
    viewport.MaxDepth = 1.0f;
    context->SetViewports(1, &viewport, 0, 0);
    context->SetPipelineState(pso);
    if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "BloomConstants")) v->Set(b.constants);
    for (const auto& [name, object] : inputs)
      if (auto* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, name)) v->Set(object);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->Draw(draw);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
  };
  // Its first mip the target of the arcs' extract; the mips after it spread them.
  TextureViewDesc first;
  first.ViewType = TEXTURE_VIEW_RENDER_TARGET;
  first.TextureDim = RESOURCE_DIM_TEX_2D;
  first.MostDetailedMip = 0, first.NumMipLevels = 1;
  RefCntAutoPtr<ITextureView> quarter_rtv;
  b.quarter->CreateView(first, &quarter_rtv);
  if (!quarter_rtv) return false;
  run(b.extract, b.extract_srb, quarter_rtv, qw, qh, {{"colorTex", color_srv}, {"depthTex", in.depth_srv}});
  ITextureView* quarter_srv = b.quarter->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
  context->GenerateMips(quarter_srv);
  run(b.apply, b.apply_srb, color_rtv, in.width, in.height, {{"bloomTex", quarter_srv}});
  return true;
}

}  // namespace native_x_renderer
