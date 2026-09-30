# Building the controller mod on a fresh PC

The scripts find their paths from where they are: the repository is two folders above
`ControllerMod\scripts`, and the folder around the repository is the work folder (it gets `logs`,
`Release`, `build-tests`, and the `TestGame` install target).

## Needs

- Windows 10/11, Git.
- Visual Studio 2022 Build Tools with "Desktop development with C++" (x86 compiler) and CMake + Ninja
  (both come with the Build Tools). Another install location: set `ZHC_VCVARS` to its
  `VC\Auxiliary\Build\vcvars32.bat`.
- For the Setup program: Inno Setup 6 (`winget install JRSoftware.InnoSetup`).
- To play: your own Command & Conquer Generals Zero Hour 1.04.

## Commands

| What | Command | Result |
| --- | --- | --- |
| Unit tests (pure input math) | `ControllerMod\scripts\test.cmd` | "N checks, 0 failed", exit code 0 |
| Test build (scripted test input ON) | `ControllerMod\scripts\build.cmd` | `build\win32\GeneralsMD\Release\generalszh.exe` |
| Player package | `ControllerMod\scripts\package.cmd <version>` (the version in `Core\GameEngine\Include\Common\ControllerModVersion.h`, e.g. 1.2.1) | `<work>\Release\ZHController-Setup-<version>.exe` and `ZHController-<version>.zip` |
| Zip installer tests | `powershell -ExecutionPolicy Bypass -File ControllerMod\tests\installer\Test-ZipInstaller.ps1` | fake game folders in %TEMP% only |
| Setup tests | `powershell -ExecutionPolicy Bypass -File ControllerMod\tests\installer\Test-Setup.ps1 -Version <version>` (after packaging) | a test copy of Setup with its own identity, silent per-user installs into fake folders in %TEMP%; `-RealIdentity -PreviousSetup <older Setup>` tests the upgrade with the released Setup |
| Multiplayer test build | `ControllerMod\tests\multiplayer\build-mptest.cmd` | `build\mptest\...\generalszh.exe` (scripted input, debug log, several copies per PC); the tests are listed in `DEVELOPMENT.md` |
| Sync test verdict (no game) | `powershell -ExecutionPolicy Bypass -File ControllerMod\tests\multiplayer\Test-MpSyncCheck.ps1` | made-up logs: each way a bad run could pass must fail |

`package.cmd` refuses a repository with uncommitted changes (a release is one fixed commit, which
VERSION.txt records in full) and always builds with `CONTROLLERMOD_TEST_INPUT=OFF`, so no environment
variable can drive a player build.

GitHub Actions is disabled on this repository. The inherited upstream workflows (including the weekly
release job) target upstream and must be removed or replaced before Actions is ever enabled here.
