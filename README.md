# SSX3-Recomp

A native PC port of **SSX 3 (original Xbox, 2003)**, made by **static recompilation**: the game's own x86
code is translated to C once with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled as a
normal x86-64 program. The Xbox kernel, GPU, audio and input are replaced by the xboxrecomp runtime and this
project's own code (a Vulkan renderer, controller support, frame interpolation). No emulator.

**Goal:** SSX 3 running natively and well on today's PCs – first on **Linux and the Steam Deck**, then Windows,
later macOS – with modern extras on top: high frame rates, higher resolutions, anti-aliasing, widescreen and
HD textures.

> **No game data is included – not in this repository and not in the releases.** You need your own, legally
> obtained copy of SSX 3 for the Xbox (see [Game files](#game-files)).

## Status

Early and experimental, but you can boot the game and race. Base version: **PAL/EU** (title ID 0x4541003C,
build 2003-09-19; English, French, German, Spanish). Other versions are not supported.

| | |
|---|---|
| Boot: EA / THX logos, intro movie (skippable) | ✅ |
| Audio (emulated Xbox APU) | ✅ |
| Title screen, menus, character select | ✅ |
| Races start and play, graphics match the reference renderer | ✅ |
| Vulkan renderer: 60 fps in menus, 55–60 fps in races | ✅ Linux |
| Frame interpolation above 60 fps (120, 144, … display refresh) | ✅ Linux |
| Internal resolution 1x–5x (up to 3200x2400) | ✅ Linux |
| Widescreen in races: 16:9, 16:10, 21:9, any ratio or the desktop's (menus stay 4:3) | ✅ Linux |
| Windowed, borderless windowed and exclusive fullscreen; Alt+Enter / F11 toggles | ✅ Linux |
| Opens on the main monitor; any other monitor selectable | ✅ Linux |
| Controllers (XInput/SDL pads, hotplug, rumble), keyboard as player 1 | ✅ |
| Start-up menu for renderer, monitor, display mode, resolution, aspect ratio, frame rate, v-sync | ✅ |
| Windows build | ⚠️ experimental: menus work, software renderer only (slow); crashes when a race loads – only tested on Linux with Wine and Proton |
| Full run through the game (all peaks, Conquer the Mountain, saving) | ⬜ not verified |
| Split screen (two pads, `RECOMP_USB_PADS=2`) | ✅ Linux: menus, race, results; ~27 fps (the game itself caps split screen at 30) |
| Steady 60 fps everywhere (the pre-race screen drops to ~40) | ⬜ in progress |
| Anti-aliasing, HD textures | ⬜ planned |
| Steam Deck test, macOS | ⬜ planned |
| Vulkan renderer on Windows (10/11 first, later 8/8.1/7) | ⬜ planned |

Known issues: an occasional crash right after the intro movie starts (start again); some rare graphics
effects are not implemented in the Vulkan renderer yet (cube maps beyond face 0, bump-env modes, palettised
textures). Full list: [docs/TODO.md](docs/TODO.md).

## Platforms

| Platform | State |
|---|---|
| **Linux** x86-64 | Main platform. Vulkan renderer, SDL2 window and controllers. |
| **Steam Deck** | The main target; should work like any Linux PC, not tested on a Deck yet. |
| **Windows** 10/11 x86-64 | Experimental, not playable yet: intro and menus run with the software (CPU) renderer, then it crashes when a race loads. So far only tested on Linux with Wine and Proton, not on real Windows. |
| **macOS** | Not started. |

## Playing a release

1. Download the package for your system from the [releases page](https://github.com/bertibertig/SSX3-Recomp/releases)
   and unpack it.
2. Put your game files into its `game_files/` folder with the installer (see [Game files](#game-files)):
   - **Linux:** `./install_game.sh`
   - **Windows:** double-click `install_game.bat` (it runs `install_game.ps1`)
3. Start the game with the launcher script:
   - **Linux:** `./run.sh`
   - **Windows:** double-click `run.bat` (it runs `run.ps1`)

   If `game_files/` is still empty, the launcher starts the installer by itself and goes on into the game
   afterwards (`SSX3_INSTALL=0` turns that off).

Linux packages need glibc 2.39 or newer (Ubuntu 24.04+, Fedora 40+, Arch, SteamOS 3.6+) and these libraries:
SDL2, OpenSSL 3, libepoxy and a Vulkan driver.

```bash
sudo apt install libsdl2-2.0-0 libssl3 libepoxy0 mesa-vulkan-drivers
```

## Game files

The game data always comes from **your own disc**; nothing of it is ever put into this repository or a release.

1. Dump your SSX 3 Xbox disc (**PAL/EU**) to an `.iso` image.
2. Run the installer: `./install_game.sh` (Linux, also `scripts/install_game.sh` in the repository) or
   `install_game.bat` (Windows). It finds disc images and extracted folders in the usual places (next to the
   program, Downloads, Desktop, Documents, Games, SD cards) or takes a path / file dialog, checks that it is the
   supported SSX 3 PAL build (title ID, region, `default.xbe` SHA-1) and copies everything into `game_files/` –
   in the repository root when you build from source, next to the program when you use a release. It accepts
   - a full Redump-style `.iso` (video + game partition) or a trimmed/rebuilt `.xiso`, read directly
     (no extra tools needed), or
   - an already extracted folder (the one with `default.xbe` in it).

   Without menus: `./install_game.sh --yes "/path/to/SSX 3.iso"`; `--check` only identifies a source.
   Extracting by hand works too, with [extract-xiso](https://github.com/XboxDev/extract-xiso) or
   [xdvdfs](https://github.com/antangelo/xdvdfs):

   ```bash
   extract-xiso -x "/path/to/SSX 3.iso" -d game_files
   ```

   In the repository, `scripts/extract_iso.sh "/path/to/SSX 3.iso"` does the same.
3. Check that the layout is right:

   ```
   game_files/
     default.xbe
     data/
     ...
   ```

If `game_files/default.xbe` is missing, the launcher starts the installer (in a terminal; otherwise it stops
with a message).

## Building from source

The build needs your own `default.xbe` (see [Game files](#game-files)): the C code is generated locally from
it (`src/recomp/gen/`) and is never committed or shipped. Two ways to build – directly on your system or in a
Docker container. Both end with the same scripts and the same `game_files/` folder in the repository root.

Get the source first (with submodules):

```bash
git clone --recurse-submodules https://github.com/bertibertig/SSX3-Recomp.git
```

```bash
cd SSX3-Recomp
```

Put your game data into `game_files/` (for both build ways; no extra tools needed):

```bash
scripts/install_game.sh "/path/to/SSX 3.iso"
```

### Build on Linux

`scripts/setup.sh` installs the build toolchain and native libraries, creates the Python virtual environment,
and installs the pipeline's Python packages. It supports Debian/Ubuntu, Arch, Fedora/RHEL, openSUSE and Alpine;
it may prompt for your password to use `sudo`. Fedora may need RPM Fusion enabled for `ffmpeg-devel`.
The Vulkan driver is used at runtime and is not installed by the setup script; install the driver appropriate
for your GPU if one is not already present. On other Linux distributions, install the equivalent packages
manually (including `glslc` from shaderc).

```bash
scripts/setup.sh
scripts/run_pipeline.sh
scripts/build.sh
scripts/run.sh
```

`run_pipeline.sh` recompiles the XBE to C (~15 min), `build.sh` compiles it to `build/ssx3_recomp` (~2 min with
16 cores), and `run.sh` starts the game with the start-up menu. Compiling the 1.4 million lines of generated C
needs a lot of RAM: use `JOBS=4 scripts/build.sh` (or lower) if the build runs out of memory.

### In a Docker container

Only Docker (or Podman) is needed on the host for the build; the toolchain lives in an Ubuntu 24.04 image
([docker/Dockerfile](docker/Dockerfile)). `scripts/docker.sh` builds the image on first use and runs a command
in it with the repository mounted, so all results land in your checkout, owned by your user.

```bash
scripts/docker.sh scripts/run_pipeline.sh
```

```bash
scripts/docker.sh scripts/build.sh
```

```bash
scripts/run.sh
```

The game itself runs on the host (it needs your display, GPU and controllers): `run.sh` picks up the
container build from `build-docker/ssx3_recomp`; the host needs the runtime libraries from
[Playing a release](#playing-a-release). `scripts/docker.sh bash` opens a shell in the container,
`SSX3_ENGINE=podman` selects Podman.

### Windows build

The Windows executable is cross compiled with MinGW-w64, on Linux or in the container:

```bash
scripts/build_win.sh
```

```bash
scripts/docker.sh scripts/build_win.sh
```

The result is `build-win/ssx3_recomp.exe` (container: `build-docker-win/`). Copy it with `scripts/run.ps1` and
your `game_files/` to a Windows PC and start `run.ps1`, or run it under Proton on Linux with `scripts/run_win.sh`.
A native MSVC build is untested.

### Unit tests and CI

The host unit tests in [tests/](tests/) need neither game data nor generated code: on a fresh clone, CMake
builds the runtime, the LAN net core and our host sources (the game executable only once `src/recomp/gen/`
exists) and `ctest` runs the tests – the packet reader/writer, the LAN net core on loopback
(`ssx3_lanbot selftest`), and on Linux the x86-64 MMIO decoder and the controller layer on a virtual pad.

```bash
cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build-test
```

```bash
ctest --test-dir build-test --output-on-failure
```

On a fresh clone, apply the runtime patches first (`scripts/build.sh` does it for you):

```bash
for p in patches/xboxrecomp/*.patch; do git -C xboxrecomp apply "../$p"; done
```

GitHub Actions
([.github/workflows/ci.yml](.github/workflows/ci.yml)) does the same on every push, on Ubuntu 24.04 (GCC) and
natively on Windows (MSYS2 MinGW-w64).

### Release packages

`scripts/package_release.sh v0.1.0` builds both packages into `dist/` (Linux in the container for
compatibility with older distributions, Windows with the host's MinGW) – without any game files.
`scripts/test_release.sh v0.1.0` then tests them like a player would: unpacked, with your `game_files/`, started
through `run.sh` and `run.ps1` (under Wine), driven into a race by a virtual controller, with frame dumps and a
summary in `logs/release-test-v0.1.0/`. The whole release (changelog, build, tests, tag, upload) is the `/release`
skill in `.claude/skills/release/` for Claude Code.

## Run options

Started from a terminal, `run.sh` first asks for the renderer (Vulkan or software, when the build has both). The
software renderer is fixed at 640x480, 4:3, 60 fps in a window, so nothing else is asked. With Vulkan it goes on
with the monitor (with more than one), the display mode (windowed, borderless, fullscreen), the internal
resolution (1x–5x), the aspect ratio (4:3, 16:9, 16:10, 21:9, auto), the frame rate (display refresh, 60 without
interpolation, 120/144/165/240, uncapped) and v-sync. The window title names the renderer in use. The choice is saved in
`~/.config/ssx3-recomp/run.conf` (Windows: `%APPDATA%\ssx3-recomp\run.conf`) and reused, also for starts
without a terminal (e.g. a Steam shortcut). Variables set in the environment win and are not asked for.

| Variable | Default | |
|---|---|---|
| `SSX3_RENDERER` | `vulkan` (Windows: `cpu`) | `cpu` = the software renderer (reference, slow); all variables below up to `SSX3_VSYNC` are Vulkan only |
| `SSX3_RES_SCALE` | `1` | internal resolution `1`–`5` × 640x480; **F10** cycles it in game |
| `SSX3_MONITOR` | `primary` | the desktop's main monitor; or a number / part of a name from `ssx3_recomp --list-displays` |
| `SSX3_WINDOW` | `windowed` | `borderless` (desktop-sized, no frame) or `fullscreen` (exclusive); **Alt+Enter** / **F11** toggle in game |
| `SSX3_ASPECT` | `4:3` | widescreen in races: `16:9`, `16:10`, `21:9`, any `W:H`, `auto` (desktop); menus stay 4:3 |
| `SSX3_FPS_LIMIT` | display refresh | interpolated frame cap in fps, `0` = uncapped |
| `SSX3_INTERP` | `1` | `0` = no interpolation, show the game's own 60 fps |
| `SSX3_VSYNC` | `0` | `1` = v-sync (FIFO); `0` = mailbox, lower latency |
| `SSX3_SKIP_LOGOS` | `1` | skip the EA and THX logos |
| `SSX3_SKIP_INTRO` | `0` | `1` = skip the intro movie too |
| `SSX3_MENU` | – | start-up menu: `0` never, `1` always |
| `RECOMP_KEYBOARD` | `1` | keyboard as controller 1 |
| `RECOMP_USB_PADS` | `1` | emulated controllers (up to 4, for split screen) |

The resolution, frame rate and v-sync settings belong to the Vulkan renderer (Linux).

## Controllers

Any XInput-style pad works: Xbox 360 / One / Series controllers, 8BitDo pads in X-input mode, the Steam
Deck's controls (through Steam Input) and anything else SDL2 knows as a game controller (on Windows: XInput).
Pads can be plugged in at any time; each takes the lowest free Xbox port. Rumble is forwarded to the pad.

| Your pad | Xbox | Keyboard (player 1) |
|---|---|---|
| A / B / X / Y | A / B / X / Y | Z / X / C / V |
| LB / RB | White / Black | Q / E |
| LT / RT | Left / right trigger | 1 / 3 |
| Start / Back | Start / Back | Enter / Backspace |
| D-pad | D-pad | Arrow keys |
| Left / right stick | Left / right stick | W A S D / I J K L |
| Stick clicks | Stick clicks | Shift / Ctrl |

- `SSX3_PAD_TRACE=1` prints every button and rumble change – handy to check a new controller.
- `config/gamecontrollerdb.txt` (or `SSX3_PAD_DB=<file>`) adds SDL mappings for unknown pads
  ([SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) format).

## Documentation

- [docs/TODO.md](docs/TODO.md) – open work and plans
- [docs/notes.md](docs/notes.md) – the running work log (addresses, findings, decisions)
- [docs/vulkan-backend.md](docs/vulkan-backend.md) – the Vulkan renderer
- [docs/frame-interpolation.md](docs/frame-interpolation.md) – frame interpolation above 60 fps

## Credits and references

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) – the recompilation toolkit and runtime (MIT, parts LGPL-2.1)
- [SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp) by Koen Hendriks and Corné Dorrestijn – an
  independent, Windows-first port of the same game; its findings were a big help
- [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC) and [OpenTricky](https://github.com/GiZcesi/OpenTricky) –
  xboxrecomp ports of the predecessor
- [ssxdecomp/ssx3](https://github.com/ssxdecomp/ssx3) – the SSX 3 PS2 decompilation
- [xemu](https://xemu.app) – the behaviour reference

## License

This project's own code is licensed under the **GNU General Public License v3.0 or later** – see
[LICENSE](LICENSE). The xboxrecomp submodule and the other third-party code keep their own licenses.

Unofficial fan project, not affiliated with or endorsed by Electronic Arts or Microsoft. SSX and SSX 3 are
trademarks of Electronic Arts. This repository contains only original code, tools and notes – no game code
or data.
