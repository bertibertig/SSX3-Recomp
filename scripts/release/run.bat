@echo off
rem SSX3-Recomp launcher: starts run.ps1 (start-up options, defaults) from this folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
if errorlevel 1 pause
