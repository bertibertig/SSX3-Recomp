@echo off
rem SSX3-Recomp game data installer: starts install_game.ps1 (copies your SSX 3 disc image or folder into game_files).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_game.ps1" %*
if errorlevel 1 pause
