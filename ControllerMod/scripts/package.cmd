@echo off
rem Builds the player version and the offline release package. All the work (and all the safety
rem checks) is in package.ps1. Usage: package.cmd 1.0.0
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" -Version "%~1"
exit /b %ERRORLEVEL%
