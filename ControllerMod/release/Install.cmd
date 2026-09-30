@echo off
rem Double-click to install the Zero Hour Controller Mod (see README.txt).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-ZHController.ps1" %*
echo.
pause
