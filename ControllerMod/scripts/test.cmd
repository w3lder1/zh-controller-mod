@echo off
rem Builds and runs the standalone controller math tests (x86, same compiler as the game).
rem Exit code 0 = all checks passed.

setlocal
rem Paths come from where this script is: the repository is two folders up, and the work folder
rem (logs, TestGame, Release) is the folder around the repository. ZHC_VCVARS can point at another
rem vcvars32.bat (Visual Studio 2022 Build Tools, x86).
for %%I in ("%~dp0..\..") do set "SRC=%%~fI"
for %%I in ("%SRC%\..") do set "ROOT=%%~fI"
if not defined ZHC_VCVARS set "ZHC_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
set OUT=%ROOT%\build-tests
if not exist "%OUT%" mkdir "%OUT%"

call "%ZHC_VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: could not load the x86 compiler environment.
    exit /b 1
)

cl /nologo /W4 /EHsc /Fe"%OUT%\ControllerMathTest.exe" /Fo"%OUT%\\" "%SRC%\ControllerMod\tests\ControllerMathTest.cpp"
if errorlevel 1 (
    echo ERROR: test build failed
    exit /b 1
)

"%OUT%\ControllerMathTest.exe"
exit /b %errorlevel%
