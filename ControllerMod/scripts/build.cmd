@echo off
rem Builds Zero Hour (generalszh.exe) from this repository
rem using VS 2022 Build Tools, 32-bit (x86), preset "win32", Release config.
rem Logs go to <work folder>\logs. Nothing is installed anywhere.

setlocal
rem Paths come from where this script is: the repository is two folders up, and the work folder
rem (logs, TestGame, Release) is the folder around the repository. ZHC_VCVARS can point at another
rem vcvars32.bat (Visual Studio 2022 Build Tools, x86).
for %%I in ("%~dp0..\..") do set "SRC=%%~fI"
for %%I in ("%SRC%\..") do set "ROOT=%%~fI"
if not defined ZHC_VCVARS set "ZHC_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
set LOGS=%ROOT%\logs
if not exist "%LOGS%" mkdir "%LOGS%"

call "%ZHC_VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: could not load the x86 compiler environment.
    exit /b 1
)

cd /d "%SRC%"

echo === Configure ===
rem RTS_INSTALL_PREFIX_ZEROHOUR is forced to the isolated copy so "cmake --install"
rem can never overwrite the retail game in Program Files.
cmake --preset win32 ^
    -DRTS_BUILD_GENERALS=OFF ^
    -DRTS_BUILD_CORE_TOOLS=OFF ^
    -DRTS_BUILD_ZEROHOUR_TOOLS=OFF ^
    "-DRTS_INSTALL_PREFIX_ZEROHOUR=%ROOT%\TestGame\ZeroHour" ^
    > "%LOGS%\configure.log" 2>&1
if errorlevel 1 (
    echo ERROR: configure failed, see %LOGS%\configure.log
    exit /b 1
)

echo === Build ===
cmake --build build\win32 --config Release --target z_generals > "%LOGS%\build.log" 2>&1
if errorlevel 1 (
    echo ERROR: build failed, see %LOGS%\build.log
    exit /b 1
)

echo === Build OK ===
exit /b 0
