// NativeXRenderer - weapon skins on the player's own first person weapon (internal).
//
// Reborn's first unlockable, the electric skin (TekRantGaming, 21 Nov 2026: "makes the gun look like electricity is
// running through it", to be unlocked later by being struck by lightning in Night Ops' storm). Only what this
// player's game draws changes: the game says which skin its own held weapon wears (SetWeaponSkin, each frame), and in
// the first person weapon's draws (DrawPath::ClassifyWeapon) its colour texture is drawn with a skinned copy of
// itself. Nothing is sent anywhere, so no other player sees it; the hands keep their own texture.
//
// The copy (WeaponSkinMake, between draws) is the gun's own colour texture made over: its metal near black with a
// faint marbling, its wood gold, and branching blue-white arcs over it that crawl, pulse and crackle, made anew 30
// times a second. It is a float texture: the arcs are twice as bright as white (no more: brighter overflows the game's
// colour target), so they stand out by night; the rain's wetness is drawn over it (WetView takes the skinned copy), and
// a subtle blue bloom round the arcs over the picture (WeaponSkinBloom). Measured on an RTX 4070 Ti at 3840 x 2160
// (native_x_renderer_gpu_sections): the copies 0.03 to 0.05 ms a frame for the Thompson, 0.07 to 0.08 for the Type 100;
// the bloom 0.05.
//
// Still to test (docs/WEAPON-SKINS.md): a true glow for the arcs, lit after the game's shading, so they stay bright in
// the dark and on a wet gun (capped at white when wet, they only just show on the Type 100 in the late Night Ops storm).

#pragma once

#include <cstdint>

#include "RefCntAutoPtr.hpp"
#include "Shader.h"

namespace Diligent {
struct IDeviceObject;
struct IRenderDevice;
struct IDeviceContext;
struct ITexture;
struct ITextureView;
}  // namespace Diligent

namespace native_x_renderer {

// The skin the game last said its held weapon wears (0 none, 1 electric), if it said so within the last quarter
// second; else 0.
int WeaponSkinNow();

// In a first person weapon's draw: the skinned copy of this texture (asked for if there is none yet), or `view` as
// it is. `hands`: the draw is the hands' (never skinned). Only the gun's colour texture is taken: a DXT1 or DXT5 2D
// texture of 256 texels or more.
Diligent::IDeviceObject* WeaponSkinView(Diligent::IDeviceObject* view, bool hands, uint64_t frame);

// Whether this is one of the skinned copies (SkinView comes before WetView: the wetness is drawn over the skin, its wet
// copy made from the skinned one).
bool IsWeaponSkinCopy(Diligent::IDeviceObject* view);

// Makes the copies asked for, and makes the ones in use anew when their time has come (30 times a second); lets go of
// the ones not drawn with for 300 frames. True if it drew anything (render targets changed).
// `wet`: how wet the weapon is now (0 to 1, the wetness's count times how much it shows): the arcs are kept dimmer wet.
bool WeaponSkinMake(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, uint64_t frame, float wet);

// The arcs' subtle blue bloom over the view's picture (once a frame, before the HUD, while the skin is worn): its
// colour (added to in place) and depth, as the depth of field takes them. NXR_SKIN_BLOOM=<strength> (0.5) and
// NXR_SKIN_BLOOM_SHOW=1 are developer's checks.
struct WeaponSkinBloomInputs {
  Diligent::ITexture* color = nullptr;
  Diligent::ITextureView* depth_srv = nullptr;
  uint32_t width = 0, height = 0;              // the view's part of the colour, in its pixels
  uint32_t depth_width = 0, depth_height = 0;  // and of the depth
  float ads = 0.0f;   // how far the player aims down the sights (0 to 1): the little clouds leave
  float dark = 0.0f;  // how dark it is (0 day, 1 night): the smoke's own shade
  bool after_night = false;  // drawn over the picture after the night pass (the picture is at its shown brightness)
  float px = 0.0f, py = 0.0f;  // the weapon's view's projection (clip x = px * view x, clip w = view z); 0: not known
};
bool WeaponSkinBloom(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, const WeaponSkinBloomInputs& in);
Diligent::RefCntAutoPtr<Diligent::IShader> CreateWeaponSkinOverShader(Diligent::IRenderDevice* device);

int TestWeaponSkinShaders();

// The storm's light on the gun's own mesh (DrawPath::DrawSkinOver): the pixel shader the gun's draws are drawn again
// with (its vertex shader the game's own, the texture's place from interpolator 7), its constants (two vec4, after
// the motion pass's one, in the same buffer) for a copy of this size, and the skinned copy as a plain 2D texture.
// The target those draws go into: the scene's depth's size (they are tested against it), made anew when it changes.
Diligent::ITextureView* WeaponSkinOverTarget(Diligent::IRenderDevice* device, uint32_t width, uint32_t height);
Diligent::ITextureView* WeaponSkinPlainView(Diligent::IDeviceObject* copy);
void WeaponSkinOverConstants(float out[12], float width, float height);
// The gun's place from its own bones, each frame it is drawn: the bone its body is on (WeaponSkinBodyBone), that
// bone's three rows of four as the vertex shader has them, the weapon's view-projection's four rows, and its
// projection's two scales (clip x = px * view x). The storm's cloud is fixed to the gun by it.
// (And the magazine's bone and its rows: when it comes back to its place after being out, a new magazine went in,
// and its charge runs over the gun.)
int WeaponSkinBodyBone();
int WeaponSkinMagazineBone();
void WeaponSkinGunFrame(const float bone[12], const float magazine[12], const float view_projection[16], float px, float py,
                        const double* view_move = nullptr);

}  // namespace native_x_renderer
