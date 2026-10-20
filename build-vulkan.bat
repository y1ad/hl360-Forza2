@echo off
rem Build FM2 with the patched Vulkan SDK used by the new FM4 launcher.
setlocal
set "FM2_ROOT=%~dp0"
for %%I in ("%~dp0..\..\fm4-recomp\work\vulkan-trial") do set "FM2_VULKAN_REFERENCE=%%~fI"
if not "%~1"=="" set "FM2_VULKAN_REFERENCE=%~f1"
if not exist "%FM2_VULKAN_REFERENCE%\host\CMakeLists.txt" (
 echo FM4 Vulkan source tree missing: %FM2_VULKAN_REFERENCE%
 exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
rem Release launchers get their online server from FM2_ONLINE_SERVER=host:port.
cmake -S "%FM2_VULKAN_REFERENCE%\host" -B "%FM2_VULKAN_REFERENCE%\build" -DFM2_ROOT="%FM2_ROOT%." "-DFM2_ONLINE_SERVER=%FM2_ONLINE_SERVER%" || exit /b 1
cmake --build "%FM2_VULKAN_REFERENCE%\build" --target forza2-vulkan Forza2LauncherVulkan -j 4
