# How the controller mod works

The controller mod is a thin layer on TheSuperHackers' GeneralsGameCode (the Community Patch
engine). This page is for people reading or changing the controller code. Building: see
[`BUILDING.md`](BUILDING.md).

## Ground rules

- **Client side only.** The controller turns pad input into the same things a mouse and keyboard
  produce: native game messages (`GameMessage` selection, order and command messages), clicks on the
  game's own windows, and the same `InGameUI` calls. It never changes game logic, INI data, saves'
  simulation state, CRCs, or network and replay formats.
- **Data driven.** Command wheels and panels read the live ControlBar and window data; there are no
  faction tables or hard-coded slot positions.
- **C++98 style**, like the surrounding engine code (no `auto`, lambdas or range-for), so the code
  could be built with the old VC6 toolchain as well.
- **Own settings file.** `ControllerMod.ini` in the game's user data folder, never `Options.ini`.
- XInput is loaded at run time (`xinput1_4.dll`, falling back to `xinput9_1_0.dll`).

## The files

All in `GeneralsMD/Code/GameEngine/Source/GameClient/Input/` unless noted.

| File | What it does |
| --- | --- |
| `GameController.cpp` (+ `Include/GameClient/GameController.h`) | The frame update: polls the pad, applies the button layout, connection and focus handling, camera and reticle movement, help overlay, diagnostics, and dispatch to the parts below |
| `XInputControllerDevice.cpp` | The XInput backend: finds and reads the pad, reports a disconnect before binding another one |
| `GameControllerSelection.cpp` | A tap / double tap / hold-to-paint selection, bumpers, RT type cycling, orders (X), guard, line move |
| `GameControllerCommandPanel.cpp` | The command panels, targeting and building placement |
| `GameControllerPanels.cpp` | Generals' powers, control groups, base and alert jumps, explicit orders |
| `GameControllerWheel.cpp` | The radial wheels (a presentation of the panels: same entries, same actions) |
| `GameControllerMenus.cpp` | The game's own menus with the D-pad: focus, activation, lists, drop-downs, sliders, tabs, Back, skipping the intro |
| `GameControllerSettings.cpp` | The controller settings screen, button layout and its emergency reset |
| `GameControllerReticle.cpp` | The reticle (presentation only: it reads hover/gesture/mode state and never input) |
| `Include/GameClient/ControllerMath.h` | Pure input math (dead zones, curves, edge offset, neighbour search, button maps, timing latches), unit-tested |
| `Core/GameEngine/Include/Common/ControllerModVersion.h` | The mod version (window title, help, packaging check) |

The engine hooks are small and marked `ControllerMod` in the code: the per-frame update and drawing
calls in `GameClient.cpp` and `InGameUI.cpp`, the radar, the camera in `W3DView`/`View.h`, placement and look-at translators, the
modal-window and tooltip helpers in `GameWindowManager` and `Mouse`, and a d3d8 loader change in
`dx8wrapper.cpp` so a GenTool `d3d8.dll` in the game folder is skipped (GenTool patches fixed
addresses of the retail exe and cannot work with this build).

## Input state

- **Contexts.** `beginInputContext()` starts a new input context whenever ownership changes
  (battlefield entered or left, help, menus). A gesture, tap, line or double-tap only finishes in the
  context it started in, and each stick must return to rest before it counts again.
- **Resets.** `resetTransientInput()` drops every half-finished action (pending bumper, held X,
  pending wheel, repeat) on mouse takeover, disconnect, focus loss and a change of pad.
- **Mouse and keyboard** always work; whichever was used last owns the reticle.
- **Menus** hold only window pointers they compare with the gadgets found this frame; drawing uses
  copies taken while the gadget was alive.

## Installer

`ControllerMod/release`: `ZHController.iss` (Inno Setup, the main download) and
`Install-ZHController.ps1` / `Uninstall-ZHController.ps1` (the zip). Rules for both:

- Only full drive-letter paths; no links or junctions on the paths used or inside what is removed.
- Never replace a `generalszh.exe` or a `ZH Controller` folder the mod did not put there.
- The zip installer keeps a record (`ZH Controller\ZHController.install.txt`, UTF-8) of every file it
  added, with its SHA-256; only files on a fixed list of names the mod has ever installed are
  accepted from it. An update records old and new versions until it has finished, so an interrupted
  update can simply be run again. The uninstaller removes only recorded, unchanged files.
- Setup records the hash of the exe it installed (`ZHController.setup.txt`) and updates only an exe
  it can prove it installed.

## Tests

| Test | Command | Covers |
| --- | --- | --- |
| Unit tests | `ControllerMod\scripts\test.cmd` | `ControllerMath.h` |
| Zip installer | `ControllerMod\tests\installer\Test-ZipInstaller.ps1` | install, update, uninstall, tampered records, locked files, non-ASCII paths, links, copy mode (fake game folders in %TEMP%) |
| Setup | `ControllerMod\tests\installer\Test-Setup.ps1 -Version <v>` | silent per-user installs into fake folders: wrong folder, EA folder layout, foreign exe/folder, take-over of a zip install, upgrade, uninstall |

**Scripted game input (test builds only).** A build configured with `CONTROLLERMOD_TEST_INPUT=ON` (the
default for `build.cmd`) reads the environment variable `CONTROLLERMOD_TEST_INPUT`, a list of timed
presses such as `300:LEFT;1500-1700:LB;1560-1900:DISC`: button names, `LSN`..`LSNW` / `RSN`..`RSNW`
for the sticks, `LT`, and `DISC` / `SWAP` to simulate unplugging or switching pads. A leading
`shell;` times the steps from the first frame (menus) instead of the first battle frame. The input
comes after the button layout, so it cannot test the physical-button emergency reset.
`CONTROLLERMOD_TEST_FACTION` forces the reticle's faction style. Player builds (`package.cmd`) are
configured with `CONTROLLERMOD_TEST_INPUT=OFF` and ignore both variables.
