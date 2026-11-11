# Third party notices

The port's own code is under the MIT License ([LICENSE](LICENSE)). That licence covers the port's own source code
only: the game's data and the code recompiled from the game's executable are not part of this repository and are not
covered by it. This repository also holds other people's code and art under their own licences: the ReXGlue SDK
(`sdk/src-bf`), NativeXRenderer (`bf/third_party/native-x-renderer`), DiligentCore and the rest below. Each keeps its
licence text in the place given here.

| Component | Where | Licence | Licence text |
|---|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), Tom Clay, derived from [Xenia](https://xenia.jp), with King Kong Recompiled's, NativeXRenderer's and this project's changes | `sdk/src-bf` | BSD 3 Clause | `sdk/src-bf/LICENSE` |
| The SDK's third party libraries, built into its runtime and tools: FFmpeg (its decoders only) and libmspack (LGPL 2.1), glslang, SPIRV Tools, SDL3, fmt, spdlog and the others in that folder | `sdk/src-bf/thirdparty` | each its own | each library's folder |
| [NativeXRenderer](https://github.com/ElGuslml/NativeXRenderer), Gustavo Garcia, with this project's changes | `bf/third_party/native-x-renderer` | MIT | `bf/third_party/native-x-renderer/LICENSE`, and its own [notices](bf/third_party/native-x-renderer/THIRD_PARTY_NOTICES.md) (DiligentCore, SPIRV Cross, volk, AMD FidelityFX, NVIDIA Image Scaling, NVIDIA DLSS, XeGTAO, SMAA, King Kong Recompiled) |
| [DiligentCore](https://github.com/DiligentGraphics/DiligentCore), Diligent Graphics: a submodule, patched when the build is configured | `bf/third_party/native-x-renderer/thirdparty/diligent-core` | Apache 2.0 | its `License.txt` |
| [miniz](https://github.com/richgel999/miniz) 3.0.2, Rich Geldreich, RAD Game Tools and Valve Software | `bf/third_party/miniz` | MIT | `bf/third_party/miniz/LICENSE` |
| Oswald, the menus' font | `bf/fonts` | SIL Open Font License 1.1 | `bf/fonts/OFL.txt` |
| Button prompts: Xelu's Free Controllers and Keyboard Prompts (Nicolae Berbece) and Input Prompts (Kenney) | `bf/glyphs` | CC0 | `bf/glyphs/LICENSE.txt` |
| [vgmstream](https://github.com/vgmstream/vgmstream): the layout of the game's menu music, as `bf/src/game_music.cpp` reads it | | ISC | `bf/licences/vgmstream.txt` |
| [GHMetalRecomp](https://github.com/ElGuslml/GHMetalRecomp), Gustavo Garcia: the NativeXRenderer profile that Battlefield's (`bf/src/native_x_renderer_bf1943.cpp`) started from | | MIT | `bf/licences/GHMetalRecomp.txt` |

The SDK's prebuilt test tools (GNU binutils for PowerPC and Cygwin, GPL) are left out of `sdk/src-bf/tools`: only the
SDK's own tests use them (`REXGLUE_BUILD_TESTS`), and this project builds without them. They are in the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) repository.

Battlefield and Battlefield 1943 are trademarks of Electronic Arts Inc. Nothing of the game is in this repository.
