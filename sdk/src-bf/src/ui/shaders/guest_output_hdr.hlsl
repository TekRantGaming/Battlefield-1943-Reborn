// SDR post-processing is complete before this pass. Linear BT.709 scRGB, 1 = 80 nits.
// Task 317 (replaces task 127's chroma-gated shoulder and task 262's log2 rise, for ALL settings):
// - at or below the knee every pixel is exactly c * paper (the SDR picture at paper white, as before);
// - above it ONE brightness curve, driven by a level that never drops when a pixel gets whiter, so a white
//   pixel is never darker than a tinted one (127 kept neutral white at paper white but pushed slightly tinted
//   near-whites up to 20x: grey cores with bright halos);
// - the curve is a BT.2390-style Hermite in PQ space: slope 1 at the knee (C1, no visible seam) rising to the
//   white target and flattening into it (no hard edge, no clipping of a full-white area);
// - RGB is scaled together (hue kept); the result never exceeds the ceiling (calibrated/DXGI peak, limited by a
//   cap on the CPU side, moving toward the full-frame level when the surroundings are bright);
// - NaN/Inf/negative inputs are sanitised. highlights 0 = no expansion at all (SDR at paper white).
struct HdrParameters {
  float paper;
  float peak;        // highlight ceiling (requested peak, limited to the display and to present_hdr_peak_cap)
  float highlights;  // 0..1: SDR white goes from paper white (0) to the ceiling (1)
  float knee;        // linear SDR level where expansion starts
  float black;       // display black level, scRGB
  float full_frame;  // ceiling for large bright areas, scRGB (<= peak)
  float text_guard;  // 0..1: white text with a dark outline (baked-in subtitles/HUD) stays at paper white
  float padding1;
};
#ifdef VULKAN
[[vk::push_constant]]
#endif
ConstantBuffer<HdrParameters> hdr_values : register(b0);
#define paper hdr_values.paper
#define peak hdr_values.peak
#define highlights hdr_values.highlights
#define knee hdr_values.knee
#define black_level hdr_values.black
#define full_frame hdr_values.full_frame
#define text_guard hdr_values.text_guard
#ifdef VULKAN
[[vk::binding(0, 0)]]
#endif
Texture2D<float4> game_image : register(t0);
#ifdef VULKAN
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> ui_image : register(t1);

// NaN and negatives -> 0, +Inf -> 1 (comparisons with NaN are false).
float3 Sanitize(float3 c) { return select(c > 0.0, min(c, 1.0), 0.0); }
float3 DecodeSrgb(float3 c) {
  c = Sanitize(c);
  return float3(c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
                c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
                c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
}
float Max3(float3 c) { return max(c.r, max(c.g, c.b)); }
float Min3(float3 c) { return min(c.r, min(c.g, c.b)); }
// Brightness used by the curve: luminance lifted halfway toward the largest component. Raising any component
// never lowers it, so whiter = at least as bright; saturated fire / coloured lights still count as bright.
float Level(float3 c) { return lerp(dot(c, float3(0.2126, 0.7152, 0.0722)), Max3(c), 0.5); }
// SMPTE ST 2084 (PQ) of an scRGB level and back.
float Pq(float v) {
  float y = pow(saturate(v * (80.0 / 10000.0)), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float PqInverse(float e) {
  float p = pow(saturate(e), 1.0 / 78.84375);
  return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125) * (10000.0 / 80.0);
}
float2 Hash2(uint2 p) {
  uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
  h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
  return float2(h & 0xffffu, h >> 16) / 65535.0;
}
// Large bright areas (sky, white fades, a wide light shaft) stay at or below the full-frame level: the ceiling
// moves from peak toward full_frame as the surroundings (12 taps on rings at 8% and 20% of the picture height)
// get brighter, so a sun or a spark can reach the ceiling but a bright area does not make the TV dim itself.
float Surroundings(int2 p, uint w, uint h) {
  float sum = 0;
  [unroll] for (int i = 0; i < 12; ++i) {
    float a = i * 0.5235988 + (i & 1) * 0.2617994;
    float r = (i & 1 ? 0.2 : 0.08) * h;
    int2 q = clamp(p + int2(float2(cos(a), sin(a)) * r), int2(0, 0), int2(w - 1, h - 1));
    sum += Level(DecodeSrgb(game_image.Load(int3(q, 0)).rgb));
  }
  return sum / 12;
}
// Baked-in guest subtitles / HUD text: thin near-neutral white strokes, dark on BOTH sides along some direction
// within a few pixels (outline, shadow or a dark scene). A plain edge (white sky next to a dark gun) is dark on one
// side only, and light shafts, sun and sky are surrounded by bright pixels: not affected. 1 = keep at paper white.
// (The presenter cannot tell HUD pixels apart: the guest image carries no HUD mask yet.)
float TextLike(float3 c, int2 p, uint w, uint h) {
  float neutral = 1.0 - smoothstep(0.03, 0.10, Max3(c) - Min3(c));
  if (neutral <= 0.0) return 0.0;
  float r = max(1.5, h * (3.0 / 720.0));
  float tap[8];
  [unroll] for (int i = 0; i < 8; ++i) {
    float a = i * 0.7853982;
    int2 q = clamp(p + int2(round(float2(cos(a), sin(a)) * r)), int2(0, 0), int2(w - 1, h - 1));
    tap[i] = Level(DecodeSrgb(game_image.Load(int3(q, 0)).rgb));
  }
  float darkest = 1.0;
  [unroll] for (int k = 0; k < 4; ++k) darkest = min(darkest, max(tap[k], tap[k + 4]));
  return neutral * (1.0 - smoothstep(0.04, 0.15, darkest));
}
// Target level (scRGB) for an SDR level L above the knee: cubic Hermite in PQ from the knee (value and slope of
// the paper-white line) to the white target (slope shrinks with the expansion, 0-like when strong: the roll-off).
// white == paper gives exactly the paper-white line.
float CurveLevel(float L, float white) {
  float a = Pq(knee * paper), s1 = Pq(paper), b = Pq(white);
  float span = max(s1 - a, 1e-6);
  float t = saturate((Pq(L * paper) - a) / span);
  float r = span / max(b - a, span);
  float t2 = t * t, t3 = t2 * t;
  float e = (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + t) * span + (-2 * t3 + 3 * t2) * b + (t3 - t2) * span * r;
  return PqInverse(e);
}
float3 MapGame(float3 gamma, uint2 p) {
  float3 c = DecodeSrgb(gamma);
  float3 mapped = c * paper;
  float L = Level(c);
  if (highlights > 0 && L > knee) {
    uint w, h;
    game_image.GetDimensions(w, h);
    // One code value of triangular dither in the expanded range breaks up banding from the 8-bit source
    // (not on clipped white, so a full-white area stays flat).
    float2 n = Hash2(p);
    float3 g = Sanitize(gamma);
    c = DecodeSrgb(g + (n.x + n.y - 1.0) / 255.0 * (1.0 - smoothstep(0.985, 1.0, Max3(g))));
    L = max(Level(c), 1e-6);
    float ceiling = full_frame < peak ? lerp(peak, full_frame, smoothstep(knee, 1.0, Surroundings(int2(p), w, h)))
                                      : peak;
    float amount = highlights * (1.0 - (text_guard > 0 ? text_guard * TextLike(c, int2(p), w, h) : 0.0));
    float white = paper + amount * max(ceiling - paper, 0.0);
    mapped = c * (CurveLevel(min(L, 1.0), white) / L);
    // Saturated colours can have a component above their level: scale RGB together to stay within the ceiling.
    mapped *= min(1.0, ceiling / max(Max3(mapped), 1e-6));
  }
  // Black lift to the display's calibrated black (BT.2390-style toe), fading out by paper white.
  float y = dot(mapped, float3(0.2126, 0.7152, 0.0722));
  float u = 1.0 - saturate(y / paper);
  return mapped + black_level * (u * u) * (u * u);
}
float4 vsmain(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
#ifdef VULKAN
  return float4(uv * 2.0 - 1.0, 0.0, 1.0);
#else
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
#endif
}
float4 psmain(float4 position : SV_Position) : SV_Target {
  int3 p = int3(position.xy, 0);
  float3 scene = MapGame(game_image.Load(p).rgb, uint2(p.xy));
  // UI was alpha blended onto transparent SDR: unpremultiply before decoding,
  // then composite in linear light at paper white.
  float4 ui = ui_image.Load(p);
  float a = ui.a > 0 ? min(ui.a, 1.0) : 0.0;
  float3 overlay = a > 0 ? DecodeSrgb(ui.rgb / max(a, 1e-6)) * a * paper : 0;
  return float4(scene * (1 - a) + overlay, 1);
}
