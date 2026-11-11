@echo off
rem Our SDK build (Battlefield 1943): as King Kong's build_win.bat, but in this folder.
rem BF_NATIVE_RENDERER_DIR: the game's native renderer (rexgpu-bfnative, Vulkan only) builds beside rexgpu-xenos.
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d %~dp0
clang --version | findstr version
cmake --preset win-amd64 -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_USE_VULKAN=ON "-DBF_NATIVE_RENDERER_DIR=%~dp0../../bf/native-renderer" || exit /b 1
cmake --build out/build/win-amd64 --config Release --target install --parallel || exit /b 1
