ZERO HOUR CONTROLLER MOD
========================

Play Command & Conquer Generals Zero Hour with an Xbox controller, Halo Wars
style: a crosshair in the middle of the screen, paint selection, radial
command wheels, and the game's own menus with the D-pad.

The controller version is an extra game program (generalszh.exe) that sits
next to your normal game. Your normal game is not changed and keeps working,
with GenTool if you use it.


WHAT YOU NEED
-------------
- Command & Conquer Generals Zero Hour 1.04, installed (tested: the EA App
  version with GenPatcher).
- Windows 10 or 11, 64-bit.
- An Xbox One / Series controller (tested: wired Xbox One controller).
- The Microsoft Visual C++ 2015-2022 Redistributable (x86). Most PCs have it;
  the installer tells you if it is missing.


INSTALL WITH THE SETUP PROGRAM (EASIEST)
----------------------------------------
1. Run ZHController-Setup-<version>.exe.
   It is not signed, so your browser and Windows may warn about it:
   - Chrome/Edge: open the downloads list (Ctrl+J) and choose Keep.
   - "Windows protected your PC": click More info > Run anyway.
2. Windows asks for permission once (the game is usually in Program Files).
3. Setup shows your Zero Hour folder (the one with INIZH.big in it). If it is
   wrong or empty, click Browse and pick it. Setup does not continue until the
   folder really is Zero Hour.
4. Start the game with the "Zero Hour Controller" shortcut (desktop or Start
   menu).

To update: run the Setup program of the new version.
To uninstall: Windows Settings > Apps > "Zero Hour Controller Mod" >
Uninstall, or Start menu > Zero Hour Controller > Uninstall.
Setup adds only generalszh.exe and the "ZH Controller" folder; Windows' list
of installed apps gets an entry for the uninstaller.


INSTALL BY HAND (THE ZIP)
-------------------------
1. Unzip the package somewhere (for example your Downloads folder).
   If you downloaded it: right-click the zip > Properties > tick "Unblock"
   first.
2. Double-click Install.cmd. Windows asks for permission once (the game is
   usually in Program Files).
3. Start the game with the new "Zero Hour Controller" shortcut on your desktop.

What the installer adds to your Zero Hour folder:
  generalszh.exe         the controller game program
  ZH Controller\         readme, controls, the uninstaller, and a list of
                         exactly what was added
Nothing of the game is replaced, renamed or changed. Nothing is written to
the registry. Nothing is downloaded.

If the installer cannot find your game it asks for the folder: paste the path
of the folder that has INIZH.big in it (for the EA App usually
C:\Program Files\EA Games\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour).
Or give it directly:  Install.cmd -GamePath "<your Zero Hour folder>"
No shortcut:       Install.cmd -NoShortcut

If a different generalszh.exe is already in the game folder (another
community build), the installer leaves it alone and stops.

Rather have a completely separate copy of the game (about 3 GB)?
  Install.cmd -Copy                     copies to %USERPROFILE%\Games\ZH Controller
  Install.cmd -Copy -InstallPath "D:\Games\ZH Controller"


UPDATE
------
Unzip the new package and run its Install.cmd again.


PLAY
----
See CONTROLS.txt, or press View in a match for the controls on screen.
Controller settings (speeds, dead zones, which button does what): press View
in any menu, or in a match View and then Y.
The normal game still starts the usual way, without the controller features.


UNINSTALL / BACK TO NORMAL
--------------------------
Installed with the Setup program: Windows Settings > Apps > "Zero Hour
Controller Mod" > Uninstall.

Installed with the zip: in your Zero Hour folder, open "ZH Controller" and double-click Uninstall.cmd,
type YES. (Uninstall.cmd in this package does the same.)
It removes exactly the files the installer added (only if they are
unchanged), the "ZH Controller" folder and the desktop shortcut.

By hand, just as good: delete generalszh.exe and the "ZH Controller" folder
from your Zero Hour folder. Nothing else was changed.

Separate copy: run Uninstall.cmd in the copy's "ZH Controller docs" folder,
or Uninstall.cmd -InstallPath "<copy folder>".

Your save games and options are kept. To also delete the controller settings:
Uninstall.cmd -RemoveControllerSettings


MULTIPLAYER (LAN AND VPN) - EXPERIMENTAL, PROBABLY DOESN'T WORK YET
-------------------------------------------------------------------
Multiplayer is included so people can try it, but it has only been tested
with two copies of the game on one PC, never between two real PCs. Expect it
not to work (games that don't connect, or that go out of sync). If you try
it, please tell us what happened (see REPORTING PROBLEMS below): that is
exactly the feedback it needs. Single player and skirmish are the parts that
are ready.

The idea: Controller Mod players play each other on a LAN, or over the
internet with a VPN such as Hamachi or Radmin VPN. Everyone needs:
- the same Controller Mod version (the same download),
- Zero Hour 1.04 without other mods. GenPatcher is fine: it does not change
  anything that matters here.
You can't play with players of the normal game, or of another Controller Mod
version: those games would go out of sync. The LAN list shows them greyed out,
with what they are ("normal ZH", "mod 1.1.0", "other files"), and joining one
tells you why it can't be joined.

Over a VPN: in Options set both "LAN IP" and "Online IP" to your VPN address
(Hamachi starts with 25., Radmin with 26.). Host with Multiplayer > Network >
Create Game. If your friend doesn't see the game in the list, use Direct
Connect: the host clicks Create Game, the other player types the host's VPN
address as Remote IP and clicks Join Game. Allow the game in Windows Firewall
(also for public networks) when Windows asks.

A multiplayer game is meant to work with the controller from the lobby to the
score screen, including chat (on-screen keyboard) and the "waiting for
players" screen. See CONTROLS.txt, "Multiplayer".


FEEDBACK AND REPORTING PROBLEMS
-------------------------------
This is an early version and feedback is very welcome: what felt good, what
felt wrong, what was hard to find, and anything that broke. Post it here:
  https://github.com/w3lder1/zh-controller-mod/issues
("Feedback" for impressions, "Bug report" for something that went wrong).
Please say which version (VERSION.txt, or the game window's title), which copy
of Zero Hour (EA App, Steam, CD) and which controller. For multiplayer tries:
LAN or VPN, how far you got, and what happened.


YOUR SAVES AND SETTINGS
-----------------------
The controller version uses the same user folder as your normal game:
  Documents\Command and Conquer Generals Zero Hour Data
Save games, options and replays are shared. Games saved with the controller
version are for the controller version: they may not load in the normal game.
The controller settings are in ControllerMod.ini in that folder.


CREDITS
-------
Almost all of this program is other people's work:
- Electronic Arts: Command & Conquer Generals Zero Hour, and the release of
  its source code under the GPL.
- TheSuperHackers and the GeneralsGameCode contributors: the Community Patch
  engine this mod is built on (modern builds, fixes and maintenance of the
  game code). https://github.com/TheSuperHackers/GeneralsGameCode
- The controller mod adds only the controller support on top: the Halo Wars
  style controls, wheels, menu navigation, reticle and the installer.
This mod is not made, supported or endorsed by Electronic Arts or by
TheSuperHackers. Please report problems with the controller mod on its own
page, not to them:
  https://github.com/w3lder1/zh-controller-mod/issues


LICENSE AND SOURCE CODE
-----------------------
The game program is built from the GPL-3.0 source code of the Zero Hour
engine (TheSuperHackers GeneralsGameCode) with the controller changes added;
see LICENSE.txt. The package contains no EA game files.
Under the GPL you are entitled to the complete source code of exactly this
version. It is published at
  https://github.com/w3lder1/zh-controller-mod
(VERSION.txt names the version; each release has its source on that page).
