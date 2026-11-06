# Changelog

## Unreleased

What the first release will hold, as of 6 November 2026.

**Play**
- The game runs at any frame rate, with the game's own 30 Hz simulation; the view, the hands and the gun move on
  every frame between its updates, and the recoil eases back on every frame.
- Online play on our servers: server browser and quick match, and no more drop after four minutes.

**Graphics (Vulkan)**
- Shadows and objects no longer flicker out.
- Antialiasing: SMAA, FXAA, DLAA or FSR 3.1 Native AA.
- Ambient occlusion and global illumination, on at Medium, with strength, quality and distance settings.
- Render resolution, shadow quality, sharpness, texture filtering and frame generation settings.
- Shaders prepared once, so effects do not stutter the first time they appear.

**Settings**
- Field of view, camera shake and its strength, hit blur.
- Master, music and voices volume.

**Known issues**
- Vehicles move at the game's 30 updates a second (smooth, no shake, but not yet at your frame rate).
- DLAA and FSR 3.1 cost about twice SMAA's GPU time.
- The effects volume setting is not available yet.
- Rejoining after a lost connection is not checked yet.
