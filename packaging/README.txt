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

The first time you start the game it reads the package you provide, checks
that it is the supported version and imports the game data from it into this
folder. After that the package is not needed any more.

Requirements:
  Windows 10 or 11 (64-bit) and a graphics card with DirectX 12 or Vulkan
  (integrated laptop graphics such as Intel HD 620 or AMD Radeon work too).
  Nothing else needs installing: the Microsoft Visual C++ runtime the game
  uses is included.

FIRST-TIME SETUP
----------------

1. Extract the release ZIP to a writable folder (not Program Files). You get
   one folder, "GoldenEye 007".
   Tip: put your package file beside "GoldenEye 007.exe", or in a folder
   called "Game Package" next to it, and it is found automatically.
2. Open "GoldenEye 007.exe". The first time, the setup window opens.
3. If your package was found it is checked and imported straight away.
   Otherwise drag the package (the 30BA9271... file) or the folder holding it
   onto the window, or click "Browse package". Setup checks it first and
   tells you if it is the wrong file or an unsupported version; the import
   starts as soon as it is accepted. You can cancel it at any time.
4. Tick any extras you want (all optional, all off unless ticked):
     Start in fullscreen, Create desktop shortcut, 100% game completion,
     Keep saves and settings in another folder ("Choose folder" picks it;
     see PORTABLE DATA).
5. Click "Play" once it says READY FOR DUTY.

From then on, opening "GoldenEye 007.exe" starts the game straight away.
Setup only reads your package; it never changes, moves or deletes it, and
it is not needed again. Nothing is installed outside this folder and no
administrator rights are needed. Setup does not download anything and
nothing contacts Xbox LIVE. The only time the game goes online on its own
behalf is when you choose Check for Updates (see UPDATING below).

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
  Gamepad Look       Modern Stick Look (off by default): the look stick
                     turns the camera directly and evenly, as the mouse
                     does, with no speed-up, no spring back when aiming or
                     letting go, and a small deadzone of your choosing.
                     Also response (linear, smooth, fine centre), look
                     speed, up/down speed, precise aim speed, deadzone and
                     outer deadzone. Works for every pad in split-screen;
                     up/down inversion stays each player's Look Up/Down
                     option. Control styles that look up/down with the
                     move stick keep the original controls.
  Keyboard & Mouse   mouse look, sensitivity, precise aim sensitivity,
                     invert look, key bindings
  Video Settings     windowed or fullscreen, window size (up to 3840x2160),
                     screen ratio (4:3, 16:9 or 21:9; the window sizes
                     offered follow it), V-Sync, render scale (1x =
                     1280x720 by default; 1.5x (new renderer), 2x and 3x
                     look sharper, need a stronger graphics card and
                     restart the game), upscaling (FSR or off), texture
                     filtering, anti-aliasing (4x by default with the new
                     renderer), post-processing
                     (colour filter), FPS counter
  Online Settings    LAN Address (which network LAN play uses), this PC's
                     address and the player name (see MULTIPLAYER)
  Other Settings     the game's own options, Camera Bob (turn off the head
                     bob while walking) and Run Setup Again
  Check for Updates  asks GitHub for a newer version (see UPDATING)

MULTIPLAYER OVER LAN OR A VIRTUAL LAN
-------------------------------------

There is no IP address to type in and no port to forward. The game finds
matches on its own network by itself, the same way System Link did on the
Xbox. It works on a home network and over a virtual LAN (Radmin VPN,
ZeroTier, Hamachi and the like). Tailscale does not pass on the broadcasts
the game uses to find matches, so it does not work.

Before you start (every player):
  1. Use the same version of this port (shown on the main menu).
  2. For a virtual LAN: join the same virtual network, and check that you
     can ping each other's virtual address (Radmin VPN shows it, e.g.
     26.x.x.x).
  3. Windows Firewall: the first time you host or join, Windows asks
     whether GoldenEye 007 may use the network. Tick both Private and
     Public (virtual LANs such as Radmin VPN usually count as public). If
     you missed it: Windows Security > Firewall & network protection >
     Allow an app through firewall > Change settings, then tick Private
     and Public for every "GoldenEye 007" entry.

Hosting: Main menu > Multiplayer > LAN or Virtual LAN > Create Match.
Joining: Main menu > Multiplayer > LAN or Virtual LAN > Join Match. The
         list keeps refreshing, so a game hosted after you opened it still
         shows up.
In the lobby the host points the crosshair at the START tab and presses A
(E on the keyboard) once everyone has joined.

Still no matches?
  - Check Help & Options > Online Settings > This PC's Address. On
    Automatic the game picks a Radmin VPN or Hamachi network itself. For
    other virtual LANs, or if it shows the wrong network, set LAN Address
    to your virtual LAN (the game restarts when you leave the page).
    Playing on a home network while Radmin VPN or Hamachi is connected?
    Disconnect it, or set LAN Address to the home network on that PC.
  - The game talks over UDP ports 1000 and 1001. Other firewall or
    antivirus programs must allow those too.
  - Each start writes a log in resources\runtime\logs. The [lan-check]
    lines show this PC's networks and its Windows Firewall rules for the
    game; the [lan] lines show the address the game used and whether
    anything arrived from the other PC. Attach the newest log from each PC
    when you report a problem.

PORTABLE DATA
-------------

Everything stays inside the extracted "GoldenEye 007" folder:

  GoldenEye 007.exe           Open this to play (and to set up the first time)
  README.txt, licenses\       This file and the third-party notices
  resources\
    runtime\                  The game program, its DLLs and the port's
                              own pictures
      logs\                   Game and setup logs
    assets\                   Game data imported from your package
    userdata\                 Saves, caches and ge.toml (display, audio and
                              control settings)

Paths are relative, so the folder can be moved or copied anywhere (a USB
drive works too). To back up your progress, copy resources\userdata.

To keep your saves and settings somewhere else (another drive, or a synced
folder), open setup (see below), tick "Keep saves and settings in another
folder" and choose a folder. When you press Play, what is in
resources\userdata is copied there (nothing already there is overwritten,
and the old copy is left as it was), and resources\userdata-location.txt
records the folder. Untick it to go back to resources\userdata. Updates
never change this. If the chosen folder is missing (a drive not connected),
the game does not start with empty saves: connect the drive, or open setup
and choose another folder.

SETUP AGAIN, UPDATING AND OLDER RELEASES
----------------------------------------

To change your extras or saves folder, or import your package again, open the setup window:
hold Shift while opening GoldenEye 007.exe (or its shortcut), or choose
Help & Options > Other Settings > Run Setup Again in the game. Starting
GoldenEye 007.exe with --setup works too. Importing again replaces resources\assets only;
saves and settings in resources\userdata are kept. If a needed file goes
missing, opening GoldenEye 007.exe shows the setup window again by itself.

UPDATING: Help & Options > Check for Updates asks this
project's GitHub page (github.com/CaiusCodes/GoldenEye-007-XBLA-PC-Recomp)
whether a newer release exists. Nothing is checked unless you choose it, and
nothing is sent but that one request (no personal data). If there is a
newer version, "Update Now" (from the main menu, not during a mission or a
network game) closes the game and opens the update window, which downloads
the release, checks it against the checksum GitHub publishes for it, and
replaces only the program files: GoldenEye 007.exe, README.txt, licenses\
and resources\runtime (not its logs). Your imported game data
(resources\assets), saves, settings and caches (resources\userdata) are
never touched, and no game data is ever downloaded. If anything goes wrong,
the old files are put back. Then click Play. Details are in
resources\runtime\logs\setup.log.

To update by hand, extract a new release over this folder (your
resources\assets and resources\userdata are kept), or into a new folder:
setup finds the old copy next to it and imports from it.

Coming from an older release (Setup GoldenEye 007.exe and a Game folder):
extract this release into the old release folder or next to it. Setup finds
the old Game folder and offers it alongside any package, so you do not need
the package again, and your saves and settings come with it. The old folder
is never changed or deleted; remove it yourself once you are happy.

TROUBLESHOOTING
---------------

Extract the ZIP first. Do not run GoldenEye 007.exe from inside the ZIP.

GoldenEye 007.exe is not digitally signed, so the first time you open it
Windows may show "Windows protected your PC". Choose More info, then Run
anyway.

If setup rejects your package, the reason is shown in the window and in
resources\runtime\logs\setup.log. When reporting a problem, include the
newest files from resources\runtime\logs.

Reporting slowdowns, stutter or freezes:
  1. Close the game. Open resources\userdata\ge.toml in Notepad and add
     this line:  performance_diagnostics = true
  2. Start the game and do what shows the problem (for example stand
     where the stutter happens for a minute or two), then close it.
  3. Send the newest ge_NNN.log from resources\runtime\logs (sort the
     folder by date). Its first lines list the settings and the PC's
     graphics card; it holds no saves, passwords or file names of yours.
  4. Change the line back to false (or delete it): the detailed log
     writes more often and is only needed while reporting.
The 20 newest logs are kept; older ones are deleted automatically.

Reporting a character that gets stuck (an ally or enemy who should walk
somewhere and doesn't, so the mission can't finish): do the same with
npc_path_log = true instead, play until it happens, wait a minute, then
close the game and send the newest log.

The renderer: the game draws with its new native renderer. The first time
each level loads, it translates the level's shaders on your PC, so that
first load can take a moment longer; later loads are quick (the results
are kept in resources\userdata\native_shaders). If the game shows wrong
or missing graphics, black screens or crashes that the classic renderer
of earlier versions did not, add this line to resources\userdata\ge.toml
to go back to it, and please report the problem with the newest log:
     gpu_native = false
Delete the line again to return to the new renderer.

LEGAL
-----

GoldenEye 007, James Bond, Xbox, Xbox 360 and related names and marks belong
to their respective owners. This is an unofficial fan project, not
affiliated with or endorsed by them. Third-party notices and license texts
are in the licenses folder.

After setup, resources\assets contains data from your own package. Do not
share the folder once you have played from it; share the release ZIP instead.

GoldenEye 007.exe in the main folder (the launcher and setup) includes a
package reader derived from Velocity and is licensed under the GNU GPL
version 3 (licenses\LICENSE-Extract-STFS.txt); its source is in the project
repository. The game in resources\runtime is a separate program under the
Unlicense and MIT licences.

resources\runtime also holds Microsoft's DirectX Shader Compiler, which the
game uses to translate shaders on your PC: dxcompiler.dll (LLVM and MIT
licences) and dxil.dll, which is Microsoft software redistributed under the
Microsoft Software License Terms. By using it you agree to those terms, in
licenses\LICENSE-MS-DirectXShaderCompiler.txt.
