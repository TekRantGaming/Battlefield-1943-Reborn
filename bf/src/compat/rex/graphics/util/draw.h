// Battlefield 1943 Reborn, SDK v0.11+ port (phase 5): v0.10's rex/graphics/util/draw.h (see rex/graphics/compat.h).
//
// v0.10's draw_util is rex::gpu::draw_state in v0.11 (gpu/xenos/draw_state.h, in rexgpu-xenos-base). Two names
// changed (ViewportInfo, DoesCoverageDependOnAlpha), and the host viewport takes the driver's gpu.xenos settings.
#pragma once

#include <cstdint>

#include "gpu/xenos/draw_state.h"
#include <rex/graphics/compat.h>
#include <rex/system/interfaces/graphics.h>

namespace rex::graphics::draw_util {

using namespace ::rex::gpu::draw_state;
using ViewportInfo = ::rex::gpu::draw_state::HostViewport;

inline bool DoesCoverageDependOnAlpha(::rex::xenos::reg::RB_COLORCONTROL rb_colorcontrol) {
  return ::rex::gpu::draw_state::IsCoverageAlphaDependent(rb_colorcontrol);
}

// The bfnative driver's settings, which exist while the renderer draws (it draws only while the driver runs).
inline void GetHostViewportInfo(const ::rex::xenos::RegisterFile& regs, uint32_t draw_resolution_scale_x,
                                uint32_t draw_resolution_scale_y, bool origin_bottom_left, uint32_t x_max,
                                uint32_t y_max, bool allow_reverse_z,
                                ::rex::xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                                bool convert_z_to_float24, bool full_float24_in_0_to_1,
                                bool pixel_shader_writes_depth, ViewportInfo& viewport_info_out) {
  ::rex::gpu::draw_state::GetHostViewport(*::rex::system::bfnative_graphics()->xenos_settings(), regs,
                                          draw_resolution_scale_x, draw_resolution_scale_y, origin_bottom_left,
                                          x_max, y_max, allow_reverse_z, normalized_depth_control,
                                          convert_z_to_float24, full_float24_in_0_to_1, pixel_shader_writes_depth,
                                          viewport_info_out);
}

}  // namespace rex::graphics::draw_util
