@echo off
rem FM2 now uses the same patched Vulkan runtime as FM4.
call "%~dp0build-vulkan.bat"
exit /b %errorlevel%
