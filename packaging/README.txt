GOLDENEYE 007 XBLA PC RECOMP
============================
Version @VERSION@

A native Windows PC version of GoldenEye 007, based on the unreleased Xbox
360 / Xbox LIVE Arcade remaster, created through static recompilation with
ReXGlue. It builds on the GoldenEye Recomp project and the GoldenEye Recomp
community fixes.

IMPORTANT
---------

This release does not contain the original game files. You need your own
copy of GoldenEye 007 XBLA: the Xbox 360 package for

  GoldenEye 007 (Xbox LIVE Arcade)
  Xbox 360 Title ID: 584108A9

Setup reads the package you provide, checks that it is the supported version
and builds the game folder from it on your PC.

Requirements:
  Windows 10 or 11 (64-bit) and a DirectX 12 capable graphics card.
  Nothing else needs installing: the Microsoft Visual C++ runtime the game
  uses is included.

FIRST-TIME SETUP
----------------

1. Extract the release ZIP to a writable folder (not Program Files). You get
   one folder, "GoldenEye 007 XBLA PC Recomp", holding Setup, this README and
   licenses.
2. Run "Setup GoldenEye 007.exe".
3. Click "Select file..." and choose your GoldenEye 007 package, or drag it
   onto the Setup window. Setup checks it straight away and tells you if it
   is the wrong file or an unsupported version.
4. Click "Install game". Setup creates a Game folder beside itself and
   unpacks your package into it.
5. Choose any extras (Start in fullscreen, 100% game completion, Desktop
   shortcut), or click Skip.
6. Click "Play now", or later run "Game\GoldenEye 007.exe".

Setup only reads your package; it never changes it. It does not download
anything and does not contact Xbox LIVE.

CONTROLS
--------

An Xbox controller works as on the console. Keyboard and mouse work at the
same time:

  W A S D           Move
  Mouse             Look and aim
  Left click        Fire
  Right click       Aim / precise aim
  E                 Activate (open doors, use objects)
  Q                 Cycle gadgets
  R                 Reload
  Mouse wheel       Cycle weapons (zooms while aiming the sniper rifle)
  F                 Toggle graphics (original / enhanced)
  Ctrl              Crouch
  Esc or Enter      Start: pause (the watch menu)
  Tab               Objectives / scores
  Arrow keys        D-pad

In the menus the mouse moves the game's crosshair, E selects (the A button)
and Esc or right click goes back. Keys can be changed in Help & Options > Keyboard & Mouse, or on the
watch's Keyboard Controls page during a mission.

SETTINGS
--------

Help & Options on the main menu holds the PC settings:
  Keyboard & Mouse   mouse look, sensitivity, precise aim sensitivity,
                     invert look, key bindings
  Video Settings     windowed or fullscreen, window size (up to 3840x2160),
                     V-Sync, frame limit, field of view, anti-aliasing,
                     texture filtering, post-processing
  Online Settings    player name and the online server options

Screen Ratio (Normal, 16:9 or 21:9) is in the game's own options.

MULTIPLAYER OVER LAN OR A VIRTUAL LAN
-------------------------------------

Main menu > Multiplayer > LAN or Virtual LAN:
  Create Match   host a game; the others join it.
  Join Match     lists the games on your network. The list keeps refreshing,
                 so a game hosted after you opened it still shows up.
In the lobby the host points the crosshair at the START tab and presses A
(E on the keyboard) once everyone has joined.

This works on a home network and over a virtual LAN (Radmin VPN, ZeroTier,
Tailscale, Hamachi and the like) when every player joins the same virtual
network. Windows asks the first time you host or join whether GoldenEye 007
may use the network; allow it on private and public networks (virtual LANs
often count as public). If the game picks the wrong network adapter, set
net_address in Game\ge.toml to your virtual LAN address.

PORTABLE DATA
-------------

Everything stays inside the extracted "GoldenEye 007 XBLA PC Recomp" folder:

  Setup GoldenEye 007.exe     Installs the game, or changes your extras later
  README.txt, licenses\       This file and the third-party notices
  Game\                       Created by Setup:
    GoldenEye 007.exe         Starts the game
    assets\                   Game data unpacked from your package
    resources\                The port's own pictures and version
    release-manifest.json     The files this release installed
    userdata\                 Saves
    ge.toml                   Display, audio and control settings
    logs\                     Game and Setup logs

To move or back up the installation, copy the entire folder.

REINSTALLING, UPDATING OR CHANGING EXTRAS
-----------------------------------------

Run "Setup GoldenEye 007.exe" again at any time. Installing again replaces
the game files Setup installed and rebuilds Game\assets from your package,
but keeps your saves (userdata) and settings (ge.toml). To update, extract a
new release over the old folder and run Setup. Setup only removes files it
recognises as its own.

With the game installed, Setup also offers:
  Change extras   Turn the extras on or off.
  Uninstall       Deletes the game and the data unpacked from your package,
                  and the desktop shortcut. Your saves and settings stay.

TROUBLESHOOTING
---------------

Extract the ZIP before running Setup. Do not run it from inside the ZIP.

Setup and the game are not digitally signed, so the first time you run Setup
Windows may show "Windows protected your PC". Choose More info, then Run
anyway.

If the game says its data is not installed, run Setup again. If Setup
rejects your package, the reason is in the newest setup-*.log in Game\logs
(or, when no Game folder was made yet, in "GoldenEye 007 Setup logs" in your
temporary folder, %TEMP%). When reporting a problem, include the newest files
from Game\logs.

LEGAL
-----

GoldenEye 007, James Bond, Xbox, Xbox 360 and related names and marks belong
to their respective owners. This is an unofficial fan project, not
affiliated with or endorsed by them. Third-party notices and license texts
are in the licenses folder.

The Game folder contains data from your own package. Do not share it.
