@echo off
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d C:\rexsrc
clang --version | findstr version
cmake --preset win-amd64 || exit /b 1
cmake --build out/build/win-amd64 --config Release --target install --parallel || exit /b 1
