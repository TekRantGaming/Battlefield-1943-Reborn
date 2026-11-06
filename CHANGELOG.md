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

**Setup and servers**
- First start setup: select your Xbox 360 package (or a folder), it installs once, then the game starts.
- Server package with a setup window: server name, map rotation (the next map when a round ends, players kept),
  the main backend, public address and router ports (UPnP); runs headless; listed for everyone with its map and players.

**Settings**
- Field of view, camera shake and its strength, hit blur.
- Master, music and voices volume.

**Known issues**
- Vehicles move at the game's 30 updates a second (smooth, no shake, but not yet at your frame rate).
- DLAA and FSR 3.1 cost about twice SMAA's GPU time.
- The effects volume setting is not available yet.
- Rejoining after a lost connection is not checked yet.
