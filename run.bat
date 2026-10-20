@echo off
setlocal
"%~dp0out\build\win-amd64-vulkan\forza2.exe" --game_data_root="%~dp0assets" --log_file="%~dp0run.log" %*
