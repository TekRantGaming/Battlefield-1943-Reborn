#pragma once
// Task 127. Windows scRGB output; no changes to the shared -041 SDK.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
#include <rex/cvar.h>
#if defined(_WIN32)
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

REXCVAR_DECLARE(bool, present_hdr);
REXCVAR_DECLARE(double, present_hdr_peak);
REXCVAR_DECLARE(double, present_hdr_paper_white);
REXCVAR_DECLARE(double, present_hdr_highlights);
REXCVAR_DECLARE(double, present_hdr_knee);
REXCVAR_DECLARE(double, present_hdr_black);
REXCVAR_DECLARE(double, present_hdr_full_frame);
REXCVAR_DECLARE(double, present_hdr_peak_cap);
REXCVAR_DECLARE(double, present_hdr_text_guard);

#define REX_UI_HDR_HIGHLIGHTS 2  // task 262: MakeConstants(..., highlights, knee, black, full_frame)
#define REX_UI_HDR_CURVE_317 1   // task 317: one monotonic curve for all settings, + peak cap and text guard
namespace rex::ui::hdr {
inline bool Eligible(bool requested, bool windows_hdr) { return requested && windows_hdr; }
template<class Formats, class Format, class ColorSpace>
inline auto FindScRgb(const Formats& formats, Format fp16, ColorSpace linear) {
  return std::find_if(formats.begin(), formats.end(), [=](const auto& f) {
    return f.format == fp16 && f.colorSpace == linear;
  });
}
struct Display {
  bool enabled = false;
  void* monitor = nullptr;
  float peak = 1000;
};
inline Display Query(void* window) {
  Display result;
#if defined(_WIN32)
  result.monitor = MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST);
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (!result.monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
  for (UINT a = 0;; ++a) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(a, &adapter))) break;
    for (UINT o = 0;; ++o) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      if (FAILED(adapter->EnumOutputs(o, &output))) break;
      Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 desc{};
      if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc)) ||
          desc.Monitor != result.monitor) continue;
      result.enabled = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
      if (std::isfinite(desc.MaxLuminance) && desc.MaxLuminance >= 80)
        result.peak = std::clamp(desc.MaxLuminance, 80.0f, 10000.0f);
      return result;
    }
  }
#endif
  return result;
}

// scRGB units (1 = 80 nits). Task 317: the shader runs ONE curve for all settings (highlights 0 = SDR at paper
// white, no expansion); text_guard sits where task 262 had padding0, so the push-constant size is unchanged.
struct Constants {
  float paper, peak, highlights = 0, knee = 0.65f, black = 0, full_frame = 0, text_guard = 0, padding1 = 0;
};
inline constexpr double kDefaultPeakCap = 1000;      // nits: a calibration/DXGI peak above it is not used
inline constexpr double kFullFrameFallback = 0.4;    // large-area ceiling = this x ceiling when full-frame is unknown
inline float FiniteClamp(double v, float lo, float hi, float fallback) {
  return std::clamp(std::isfinite(v) ? float(v) : fallback, lo, hi);
}
// paper / peak: requested nits (peak limited to the display's reported peak and to peak_cap). Task 262/317:
// highlight expansion 0..1 (SDR white goes from paper white to the ceiling), knee (linear SDR level kept exactly
// at paper white below it), the display's black level and full-frame (large area) brightness in nits. Full-frame
// unknown (0) or equal to peak (TVs that report the same value for both) -> kFullFrameFallback x the ceiling.
inline Constants MakeConstants(double paper, double peak, double display_peak, double highlights = 0,
                               double knee = 0.65, double black = 0, double full_frame = 0,
                               double peak_cap = kDefaultPeakCap, double text_guard = 1) {
  const float display = FiniteClamp(display_peak, 80, 10000, 1000);
  const float requested = FiniteClamp(peak, 80, display, display);
  const float top = std::min(requested, FiniteClamp(peak_cap, 80, 10000, float(kDefaultPeakCap)));
  const float p = FiniteClamp(paper, 80, std::min(500.0f, top), 200);
  Constants c{p / 80, top / 80};
  c.highlights = FiniteClamp(highlights, 0, 1, 0);
  c.knee = FiniteClamp(knee, 0.2f, 0.9f, 0.65f);
  c.black = FiniteClamp(black, 0, 5, 0) / 80;
  float ff = FiniteClamp(full_frame, 0, 10000, 0);
  if (ff <= 0 || ff >= 0.95f * requested) ff = float(kFullFrameFallback) * top;
  c.full_frame = std::clamp(ff, p, top) / 80;
  c.text_guard = FiniteClamp(text_guard, 0, 1, 1);
  return c;
}
struct Connection {
  void* window = nullptr;
  Display display;
  bool requested = false;
  bool active = false;
  std::chrono::steady_clock::time_point checked{};
  void Connect(void* hwnd) {
    window = hwnd;
    display = Query(hwnd);
    requested = REXCVAR_GET(present_hdr);
    active = Eligible(requested, display.enabled);
    checked = std::chrono::steady_clock::now();
  }
  bool Changed() {
    const bool want = REXCVAR_GET(present_hdr);
    if (want != requested) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::seconds(1)) return false;
    checked = now;
    const auto next = Query(window);
    return next.monitor != display.monitor || next.enabled != display.enabled || next.peak != display.peak;
  }
  Constants Values() const {
    return MakeConstants(REXCVAR_GET(present_hdr_paper_white), REXCVAR_GET(present_hdr_peak), display.peak,
                         REXCVAR_GET(present_hdr_highlights), REXCVAR_GET(present_hdr_knee),
                         REXCVAR_GET(present_hdr_black), REXCVAR_GET(present_hdr_full_frame),
                         REXCVAR_GET(present_hdr_peak_cap), REXCVAR_GET(present_hdr_text_guard));
  }
};
}  // namespace rex::ui::hdr
