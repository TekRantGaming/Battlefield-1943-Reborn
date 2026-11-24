// NativeXRenderer - a developer's export of the first person weapons' models (internal; compiled in only with
// NXR_INTERNAL_TOOLS, which this PC's gitignored bf/local.cmake sets: without it every function here does nothing).
//
// The user, 22 Nov 2026: "If only we could dump all the weapon models for making completely custom textures and
// shaders + effects for the future too that we'd be able to distribute back to users as the unlocks for certain
// events or rewards"; and of the weapon skin's cloud, placed by the gun's outline in the picture: "I thought you
// dumped the models to get this right"; "Let's pause this work then until the weapons are dumped".
//
// NXR_WEAPON_EXPORT=<folder>: each first person weapon the player holds is written there once, in a folder of its
// name (the game's: thompson, nambu100, ...), as it is drawn:
//   weapon.obj   its mesh, every part put together as held, in the space of its main part (metres), with its texture
//                coordinates and normals; a group for each draw and bone;
//   info.txt     each draw (shaders, vertices, triangles, textures), each bone's part (its box, how many vertices), the
//                whole gun's box and its long axis: where its muzzle, receiver and magazine are;
//   bones.txt    the bones' places against the main part's, as held;
//   tex_*.dds    its textures as the game has them (colour, relief, masks).
// These are the game's own assets: references for making skins of our own, kept on this PC, never put into the
// repository or a release.

#pragma once

#include <cstdint>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITextureView;
}  // namespace Diligent

namespace native_x_renderer {

bool WeaponExportOn();

// The name of the weapon the player holds now (the game's, in lower case; the public NoteHeldWeapon).
void WeaponExportName(const char* name);

struct WeaponExportDraw {
  uint64_t vs_hash = 0, ps_hash = 0;
  const float* vs_constants = nullptr;  // the vertex shader's float constants c0 to c255 (four floats each)
  const uint8_t* vertices = nullptr;    // the vertex buffer in the game's memory (big endian words)
  uint32_t vertex_bytes = 0, stride_bytes = 0;
  int position = -1, bone = -1, uv = -1, normal = -1;  // byte offsets in a vertex (-1: none)
  uint32_t vertex_address = 0, index_address = 0;
  const uint8_t* indices = nullptr;  // big endian; null: not indexed
  uint32_t index_count = 0;
  bool index_32 = false;
  uint32_t primitive = 0;  // the Xenos primitive type (4 a triangle list, 6 a strip)
  // Its pixel shader's textures: the fetch constant, the guest's size and format, the host's view.
  struct Texture {
    uint32_t fetch, width, height, format;
    Diligent::ITextureView* view;
  };
  const Texture* textures = nullptr;
  uint32_t texture_count = 0;
};
void WeaponExportDrawNow(const WeaponExportDraw& draw);

// The textures' pixels read back and written (where the device context may be used directly, once a frame).
void WeaponExportFlush(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);

}  // namespace native_x_renderer
