@echo off
rem Builds the multiplayer TEST version (build\mptest): scripted controller input, the state file,
rem debug logging with sync checksums, and several game copies per PC. Never for players.
rem Paths as in ControllerMod\scripts\build.cmd: the repository is three folders up, logs go to
rem <work folder>\logs. ZHC_VCVARS can point at another vcvars32.bat.
setlocal
for %%I in ("%~dp0..\..\..") do set "SRC=%%~fI"
for %%I in ("%SRC%\..") do set "ROOT=%%~fI"
if not defined ZHC_VCVARS set "ZHC_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
set LOGS=%ROOT%\logs
if not exist "%LOGS%" mkdir "%LOGS%"
call "%ZHC_VCVARS%" >nul
cd /d "%SRC%"
cmake --preset win32 -B build\mptest -DRTS_BUILD_GENERALS=OFF -DRTS_BUILD_CORE_TOOLS=OFF -DRTS_BUILD_ZEROHOUR_TOOLS=OFF -DRTS_DEBUG_MULTI_INSTANCE=ON -DRTS_DEBUG_LOGGING=ON -DRTS_DEBUG_CRASHING=OFF -DCONTROLLERMOD_TEST_INPUT=ON "-DRTS_INSTALL_PREFIX_ZEROHOUR=%ROOT%\install-unused" > "%LOGS%\mptest-configure.log" 2>&1
if errorlevel 1 exit /b 1
cmake --build build\mptest --config Release --target z_generals > "%LOGS%\mptest-build.log" 2>&1
exit /b %errorlevel%