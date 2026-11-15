@echo off
rem Starts GTA V through RAGE Plugin Hook. Use this instead of the launcher's Play button.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rph-files.ps1" -Start
if errorlevel 1 pause
