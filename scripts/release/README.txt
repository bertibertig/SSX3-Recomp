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

Dump the disc to an .iso image, then let the installer copy it into the game_files
folder next to this file:

    Linux:    ./install_game.sh
    Windows:  double-click install_game.bat

It finds disc images (.iso, .xiso) and already extracted folders in the usual places
or asks for a path, checks that it is the supported PAL version and copies the game
data. No extra tools needed. run.sh / run.bat also start it by themselves when
game_files is still empty. Without menus:

    ./install_game.sh --yes "SSX 3.iso"

Extracting by hand works too, with extract-xiso (https://github.com/XboxDev/extract-xiso)
or xdvdfs (https://github.com/antangelo/xdvdfs):

    extract-xiso -x "SSX 3.iso" -d game_files
    xdvdfs unpack "SSX 3.iso" game_files

Afterwards the folder looks like this:

    SSX3-Recomp-.../
      ssx3_recomp          (ssx3_recomp.exe on Windows)
      run.sh               (run.ps1 / run.bat on Windows)
      install_game.sh      (install_game.ps1 / install_game.bat on Windows)
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

  Settings: ./run.sh --settings opens the settings screen first: resolution
  (1x-5x), aspect ratio (4:3, 16:9, 16:10, 21:9, auto), frame rate, v-sync,
  window mode, monitor, renderer, language (automatic = your system's, or
  English, Francais, Deutsch, Espanol) and start-up options. Enter saves and
  starts. Saved in ~/.config/ssx3-recomp/settings.ini and used on every start,
  also without a terminal (e.g. as a Steam shortcut).

Windows (10 or 11, x86-64): double-click run.bat (or run
  powershell -ExecutionPolicy Bypass -File run.ps1 in a console;
  add --settings for the settings screen, saved in
  %APPDATA%\ssx3-recomp\settings.ini). Needs a Vulkan driver (any current
  NVIDIA, AMD or Intel driver); without one it falls back to the slow software
  renderer.
  - So far tested on Linux with Proton (menus and races), not yet on a real
    Windows PC. Reports from Windows 10/11 are welcome.


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
  F11 or Alt+Enter = toggle windowed / full screen (Vulkan)


4. Options (environment variables)
----------------------------------

These override the settings file for one run. All options except language and
logo/intro belong to the Vulkan renderer.

  SSX3_LANGUAGE=en|fr|de|es   the game's language (default: the system's)
  SSX3_MONITOR=primary|<number>|<name>  monitor (default: the desktop's main one;
                        ./ssx3_recomp --list-displays lists them)
  SSX3_WINDOW=windowed|borderless|fullscreen   display mode
  SSX3_RES_SCALE=1..5   internal resolution, multiple of 640x480
  SSX3_ASPECT=4:3|16:9|16:10|21:9|W:H|auto     widescreen in races (menus stay 4:3)
  SSX3_FPS_LIMIT=<n>    frame cap with interpolation (default: display refresh, 0 = uncapped)
  SSX3_INTERP=0         no frame interpolation, show the game's own 60 fps
  SSX3_VSYNC=1          v-sync on
  SSX3_SKIP_LOGOS=0     show the EA and THX logos (skipped by default)
  SSX3_SKIP_INTRO=1     skip the intro movie too
  SSX3_RENDERER=cpu     software renderer (reference, slow)


5. Known issues
---------------

- Early release: a full run through the game (all peaks, Conquer the Mountain,
  saving) and split screen are not verified yet.
- An occasional crash right after the intro movie starts; just start again.
- Some screens (e.g. before a race) run below 60 fps.
- Some rare graphics effects are not implemented in the Vulkan renderer yet.
- The rider's shadow occasionally glitches.
- Widescreen is not checked in split screen and replays yet.

Bug reports: https://github.com/bertibertig/SSX3-Recomp/issues
Please attach the console output (./run.sh 2>&1 | tee run.log).
