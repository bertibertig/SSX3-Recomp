SSX3-Recomp @VERSION@
=====================

A native PC port of SSX 3 (original Xbox, 2003), made by statically recompiling the
game's Xbox code to C. No emulator. Early, experimental release.

Project page and source: https://github.com/bertibertig/SSX3-Recomp
License: GPL-3.0-or-later (LICENSE.txt); third-party licenses in licenses/.

Unofficial fan project, not affiliated with or endorsed by Electronic Arts or Microsoft.


1. You need your own copy of the game
-------------------------------------

This package contains NO game data. You need a legally obtained SSX 3 disc for the
original Xbox, PAL/EU version (title ID 4541003C, English/French/German/Spanish).
Other versions (NTSC-U, NTSC-J) are not supported.

Dump the disc to an .iso image, then extract it into the game_files folder next to
this file, with extract-xiso (https://github.com/XboxDev/extract-xiso) or
xdvdfs (https://github.com/antangelo/xdvdfs):

    extract-xiso -x "SSX 3.iso" -d game_files
    xdvdfs unpack "SSX 3.iso" game_files

Afterwards the folder looks like this:

    SSX3-Recomp-.../
      ssx3_recomp          (ssx3_recomp.exe on Windows)
      run.sh               (run.ps1 / run.bat on Windows)
      game_files/
        default.xbe
        data/
        ...


2. Start the game
-----------------

Linux (x86-64, glibc 2.39 or newer: Ubuntu 24.04+, Fedora 40+, Arch, SteamOS 3.6+):

    ./run.sh

  Needs SDL2, OpenSSL 3, libepoxy and a Vulkan driver, e.g.
    Ubuntu/Debian: sudo apt install libsdl2-2.0-0 libssl3 libepoxy0 mesa-vulkan-drivers
    Arch:          sudo pacman -S sdl2 openssl libepoxy vulkan-icd-loader  (+ your GPU's Vulkan driver)
    Fedora:        sudo dnf install SDL2 openssl-libs libepoxy mesa-vulkan-drivers

  Started from a terminal, run.sh first shows a menu for the internal resolution
  (1x-5x), the frame rate and v-sync. The choice is saved in
  ~/.config/ssx3-recomp/run.conf and reused, also without a terminal (e.g. as a
  Steam shortcut). SSX3_MENU=1 ./run.sh shows the menu again.

Windows (10 or 11, x86-64): double-click run.bat (or run
  powershell -ExecutionPolicy Bypass -File run.ps1 in a console).
  Windows is experimental and not playable yet:
  - So far it has only been tested on Linux with Wine and Proton, not on a
    real Windows PC. Reports from Windows 10/11 are welcome.
  - Under Wine and Proton, intro, title and menus work, then it crashes when a
    race loads.
  - It uses the software (CPU) renderer (about 10-20 fps); the fast Vulkan
    renderer is Linux-only so far.


3. Controls
-----------

Any XInput-style controller works (Xbox 360/One/Series, 8BitDo in X-input mode,
Steam Deck controls through Steam Input, most SDL-supported pads). Pads can be
plugged in at any time. LB/RB = White/Black, triggers are analog, rumble works.

Keyboard (player 1):
  Enter = Start         Backspace = Back        Arrows = D-pad
  Z / X / C / V = A / B / X / Y                 Q / E = White / Black
  1 / 3 = left / right trigger
  W A S D = left stick  I J K L = right stick   Shift / Ctrl = stick clicks
  F10 = cycle the internal resolution (Vulkan)


4. Options (environment variables)
----------------------------------

The resolution, frame rate and v-sync options belong to the Vulkan renderer (Linux).

  SSX3_RES_SCALE=1..5   internal resolution, multiple of 640x480
  SSX3_FPS_LIMIT=<n>    frame cap with interpolation (default: display refresh, 0 = uncapped)
  SSX3_INTERP=0         no frame interpolation, show the game's own 60 fps
  SSX3_VSYNC=1          v-sync on
  SSX3_SKIP_LOGOS=0     show the EA and THX logos (skipped by default)
  SSX3_SKIP_INTRO=1     skip the intro movie too
  SSX3_RENDERER=cpu     software renderer (reference, slow)
  SSX3_MENU=0 / 1       start-up menu never / always


5. Known issues
---------------

- Early release: a full run through the game (all peaks, Conquer the Mountain,
  saving) and split screen are not verified yet.
- An occasional crash right after the intro movie starts; just start again.
- Some screens (e.g. before a race) run below 60 fps.
- Some rare graphics effects are not implemented in the Vulkan renderer yet.

Bug reports: https://github.com/bertibertig/SSX3-Recomp/issues
Please attach the console output (./run.sh 2>&1 | tee run.log).
