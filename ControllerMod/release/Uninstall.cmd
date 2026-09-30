@echo off
rem Double-click to uninstall the Zero Hour Controller Mod (see README.txt). In the game folder's
rem "ZH Controller" folder it removes the controller from that game. Everything is on one line: cmd
rem reads the whole line before running it, so it does not need this file again after it is removed.
cd /d "%TEMP%" & powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-ZHController.ps1" %* & echo. & pause & exit /b
