@echo off
rem Build with the pinned SDK in ..\sdk. Puts Visual Studio's LLVM clang first on PATH (another clang on PATH
rem breaks the build). Usage: build.bat [bf-debug^|bf-release^|bf-relwithdebinfo^|bf-dev]
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=bf-relwithdebinfo
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
