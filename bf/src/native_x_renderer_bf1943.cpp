// NativeXRenderer for Battlefield 1943 (bf/third_party/native-x-renderer, MIT): the game's XDK Direct3D entry points
// and its device and object layouts. DECISIONS.md, 5 Nov 2026: our native Vulkan renderer is built on it.
//
// The game links XDK 2.0.7978, the version of Guitar Hero: Metallica's profile (GHMetalRecomp, MIT): the layouts below
// are that version's (its src/native_x_renderer_ghmetallica.cpp and docs/native-x-renderer-ghmetallica.md), checked
// for this executable in docs/native-x-renderer-bf1943.md and by the observer (--native_x_renderer_verify). The hooks
// only run the original function unless a NativeXRenderer feature is on (--native_x_renderer_trace,
// --native_x_renderer_verify, --native_x_renderer).

#include <filesystem>

#include <native_x_renderer/hooks.h>
#include <native_x_renderer/native_x_renderer.h>

// One line per entry point, with its evidence (docs/native-x-renderer-bf1943.md).
#include "native_x_renderer_bf1943_hooks.inc"

namespace bf1943 {

const native_x_renderer::Profile& NativeXRendererProfile() {
  static const native_x_renderer::Profile profile = [] {
    native_x_renderer::Profile p;
    p.game = "Battlefield 1943 (5841097E)";
    p.xdk_version = "2.0.7978";
    p.output_width = 1280;
    p.output_height = 720;
    // The finished 3D scene's copy (run gctrace1, frame 8000 dumped on Guadalcanal): the first colour resolve of the
    // output size through the engine's resolve function 0x82586834 (from 0x825F00F8), after the first person weapon
    // and before the bloom chain (its other resolves, 640x360 down to 1x1, are smaller) and the HUD. SMAA or FXAA run
    // on it (scene_resolve_caller), and DLSS or FSR 3.1 end the main view there (D119). Without them the renderer had no
    // antialiasing and no upscaler at all ("the game's profile names no scene resolve").
    p.scene_resolve_caller = 0x82586834;
    p.upscale_resolve_caller = 0x82586834;
    // The ambient occlusion's moment (D132): the output-size depth copy through 0x825868A4 (from 0x825EF498), right
    // after the scene's opaque draws, before its water, particles and the first person weapon (drawn after a depth
    // clear), so those are never darkened (run gcrd2: opaque draws to 1645, the depth copy, blended draws from 1647).
    p.ao_resolve_caller = 0x825868A4;
    // D165: the game's own undergrowth (TekRantGaming, 13 Nov 2026: "the extra vegetation starts to behave strangely
    // and starts appearing on roads and places it should not be", from a second map on). The game draws a 64 x 64
    // mask for each undergrowth cell, resolves it, and its jobs read the texture's memory to place plants. The
    // renderer keeps resolves on the host, so the jobs read what the memory held before: zeros in a first level (no
    // game undergrowth: the look players know, with our own grass), another level's leftovers from the second one
    // (run veg1: Wake Island's 300 masks all zero, Guadalcanal's after it 15973 bytes of 16384 set): plants on
    // roads, in yards, in black. The masks' memory is set to zero as the game resolves them, so every level is as
    // the first one was. Every resolve goes through the game's own function (the caller here is its, as the scene
    // copy's): the masks are known by their size and format.
    p.zeroed_resolves[0] = {0x82586834, 0, 64, 64, 6};
    // D164 (TekRantGaming, 12 Nov 2026: a black outline round the ships against the water, a white glow round palm
    // leaves and buildings): the game's pixel shaders that read their pixel position sample at it plus one texel (the
    // water's depth read at (VPOS + 1) / 1280, run rdship1: two host rows above a landing craft's hull read the hull's
    // depth and drew the water black), so with a render scale they get the host pixel's centre less one guest pixel.
    p.param_gen_texel_centre = true;
    // Ultrawide (0.4.0): the 3D view fills a screen of any shape, its cameras widened by AspectFactor() (fov.cpp's
    // setAspect hook), and the HUD drawn after the composite kept at 16:9 in the middle (D43, D162's frame order).
    p.widens_cameras = true;
    p.box_2d_after_composite = true;
    native_x_renderer::DeviceLayout& d = p.device;
    // XDK 2.0.7978 (Guitar Hero: Metallica's): six dirty masks from +0 (VS constants, PS constants, register groups
    // and the program, fetch slots with slot 0 in bit 31, the 0x2300/0x2380 groups, the tile dependent state).
    d.vs_constants_dirty = 0;
    d.ps_constants_dirty = 8;
    d.fetch_constants_dirty = 24;
    d.fetch_dirty_slot0_bit = 31;
    d.command_buffer_write = 48;
    d.fetch_constants = 1152;
    d.vs_float_constants = 1920;
    d.ps_float_constants = 6016;
    d.bool_loop_constants = 10112;
    d.register_groups = {{
        {10368, 0x2000, 16},
        {10444, 0x2100, 21},
        {10528, 0x2180, 5},
        {10548, 0x2200, 12},
        {10596, 0x2280, 21},
        {10680, 0x2300, 38},
        {10832, 0x2380, 8},
    }};
    d.fence_counter = 10908;
    d.write_back_block = 10896;
    d.flags = 10940;
    d.vertex_declaration = 11824;
    d.index_buffer = 12444;
    d.render_targets = 12448;
    d.depth_stencil = 12464;
    d.vertex_buffers = 12468;
    d.textures = 12552;
    d.viewport = 12656;
    d.pixel_shader = 12700;
    d.vertex_shader = 12704;
    d.predication_mask = 12716;
    d.tile_count = 12756;
    d.tile_rects = 12760;
    // DrawVerticesUP, DrawIndexedVerticesUP and EndVertices commit BeginVertices' / BeginIndexedVertices' packets with
    // [dev+48] = [dev+13444].
    d.inline_draw_end = 13444;
    // The engine records draw lists on command buffer devices (Direct3D_CreateDevice type 2, which sets +10940 |=
    // 0x80) on its job threads, and runs them on the main device (RunCommandBuffer, sub_82177CF8): their calls are
    // left to the main device's run (run nxr2: 371 of 620 indexed draws a frame recorded so).
    d.recording_flags = 10940;
    d.recording_mask = 0x80;
    native_x_renderer::ObjectLayout& o = p.objects;
    o.texture_fetch = 28;
    o.surface_info = 24;
    o.surface_color_depth_info = 28;
    o.surface_size = 36;
    o.surface_width_minus_one = {18, 14};
    o.surface_height_minus_one = {3, 15};
    o.buffer_address = 24;
    o.buffer_size = 28;
    return p;
  }();
  return profile;
}

}  // namespace bf1943
