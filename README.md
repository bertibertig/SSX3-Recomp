# SSX3-Recomp

Experimental **static recompilation of SSX 3 (original Xbox, 2003)** into a native x86-64 PC program,
based on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (Git submodule under `xboxrecomp/`).
The game's own code is translated to C once and compiled natively; the xboxrecomp runtime and our own
code replace the Xbox kernel, GPU, audio and input. No emulator.

> This repo contains **no game code and no game data**. You need your own, legally obtained
> SSX 3 Xbox disc image. The C code is generated locally from your `default.xbe` and never committed.

## Platforms: Linux first

This is a **Linux-first port**, mainly with the **Steam Deck** in mind: native Linux build, Vulkan
renderer, SDL2 window and controllers.

| Platform | State |
|---|---|
| **Linux** (x86-64, Steam Deck target) | Main platform. Builds with GCC/Clang, Vulkan renderer, SDL2 input. Not yet tested on a Deck itself. |
| **Windows** | Planned as the next platform. xboxrecomp itself is Windows-first (D3D11, XInput, XAudio2), so most of the runtime is already there. Today: MinGW cross build from Linux, tested under Proton; native MSVC build and the Vulkan back end's Win32 surface are still open. |
| **macOS** | Planned, later. Not started (Vulkan via MoltenVK; Apple Silicon needs extra MMIO work). |

## Status

Our base is the PAL/EU build (title ID 0x4541003C, 2003-09-19; English, French, German, Spanish).

| Milestone | |
|---|---|
| Pipeline: 17,017 functions lifted, 0 translation errors, 1.37 M lines of C | ✅ |
| Linux build (GCC 16, ~1:40 min) and Windows cross build (MinGW, runs under Proton) | ✅ |
| Boot: EA BIG / THX logos, intro movie (skippable), boot in ~0.5 s | ✅ |
| Audio through the emulated APU (DirectSound) | ✅ |
| Title screen, main menu, character select | ✅ |
| **Race starts and plays**, in-game graphics match the CPU reference renderer | ✅ |
| Vulkan renderer: **60 fps** title/menus, **55–60 fps** in races (CPU renderer: ~15 fps) | ✅ |
| Controllers: XInput/SDL pads with hotplug, keyboard as player 1 | ✅ |
| Full game run-through (all peaks, Freestyle, Conquer the Mountain, saving) | ⬜ not verified yet |
| Split screen (2 pads) | ⬜ untested |
| Frame interpolation above 60 fps: menus and races shown at the display's refresh (tested 120 and 270 Hz) while the game simulates at 60 | ✅ (Vulkan, default; `SSX3_INTERP=0` off) |
| Steady 60 game fps everywhere (pre-race screen ~40 fps) | ⬜ in progress |
| Higher internal resolution: 2x–5x (`SSX3_RES_SCALE`, F10) | ✅ (Vulkan) |
| AA, widescreen, HD textures | ⬜ planned |
| Steam Deck test, Windows native build, macOS | ⬜ planned |

Known issues: an intermittent crash right after the intro/attract movie starts (`sub_00177F80`);
the vblank clock runs at ~50 Hz by accident; some Vulkan features are missing (cube maps beyond face 0,
bump-env modes, palettised textures). Details and the full list: [docs/TODO.md](docs/TODO.md),
the running log: [docs/notes.md](docs/notes.md), the renderer: [docs/vulkan-backend.md](docs/vulkan-backend.md).

## Quick start (Linux)

```bash
git clone --recurse-submodules <this-repo>       # or locally: git submodule update --init --recursive
cd SSX3-Recomp

scripts/setup.sh                                  # Python venv + capstone, tool check
bash xboxrecomp/tools/linux/install_deps.sh       # SDL2, libepoxy, OpenSSL, ffmpeg (sudo); Vulkan driver + glslc needed too

scripts/extract_iso.sh "/path/to/SSX 3 (Xbox).iso"   # -> game_files/default.xbe + data
scripts/run_pipeline.sh                           # parse, disasm, func_id, abi, recomp -> src/recomp/gen/ (~15 min)
scripts/doctor.sh                                 # bring-up report
scripts/build.sh                                  # CMake build (applies the xboxrecomp runtime patch) -> build/ssx3_recomp
scripts/run.sh                                    # start the game
```

Compiling the generated code needs a lot of RAM: lower `JOBS=` if the build runs out of memory.

Windows build (cross compiled on Linux, run under Proton):

```bash
scripts/build_win.sh
```

```bash
scripts/run_win.sh
```

### Run options

`scripts/run.sh` sets the defaults the game needs; these are the ones you may want to change:

| Variable | Default | |
|---|---|---|
| `SSX3_RENDERER` | `vulkan` | `cpu` = the software rasteriser (reference, slow) |
| `SSX3_SKIP_LOGOS` | `1` | skip the EA BIG and THX logos |
| `SSX3_SKIP_INTRO` | `0` | `1` = skip the intro movie too |
| `SSX3_RES_SCALE` | `1` | internal resolution `1`–`5` × 640x480 (2 = 1280x960 … 5 = 3200x2400); **F10** cycles it in game |
| `RECOMP_KEYBOARD` | `1` | keyboard as controller 1 (Enter = Start, Z = A, X = B, arrows = D-pad …) |
| `RECOMP_USB_PADS` | `1` | emulated pads on the USB bus (up to 4, for split screen) |

## Controllers

Any XInput-style pad works on Linux and Windows: Xbox 360 / One / Series controllers, 8BitDo pads in
X-input mode, the Steam Deck's built-in controls (through Steam Input – not yet tested on a Deck), and anything else SDL2 knows as a
game controller (on Windows: XInput). Pads can be plugged in or switched on at any time; each takes the
lowest free Xbox port (`PAD: port 1 <- "…"` in the log). The keyboard keeps working on port 1 alongside a pad.

| Your pad | Xbox (Controller S) |
|---|---|
| A / B / X / Y | A / B / X / Y |
| LB / RB | White / Black |
| LT / RT | Left / right trigger (analog) |
| Start (Menu) / Back (View) | Start / Back |
| Sticks, stick clicks, D-pad | Same |

- Rumble: the game's force feedback goes to the pad's motors (SDL rumble / XInput vibration).
- `SSX3_PAD_TRACE=1` prints every button and rumble change per port – handy to check a new controller.
- `config/gamecontrollerdb.txt` (or `SSX3_PAD_DB=<file>`) adds SDL mappings for unknown pads
  ([SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) format).
- `scripts/test_pad.sh` checks hotplug and the map with a virtual pad, no hardware needed.

Code: `src/input/ssx3_pad.c` (replaces the runtime's `xinput_device.c`). Open: PlayStation/Switch
specifics, rebinding – see [docs/TODO.md](docs/TODO.md).

## How it works

1. `scripts/run_pipeline.sh` runs the xboxrecomp tools on your `default.xbe`: XBE parsing, disassembly,
   function identification, ABI analysis and the lift to C (`src/recomp/gen/`, never committed).
2. Hand-written code fills the gaps: `src/main.c` (host entry for Windows and POSIX), `src/recomp_manual.c`
   (overrides), `config/seed_functions.json` (entry points the static analysis misses), `src/render/`
   (Vulkan back end for the emulated NV2A GPU), `src/input/` (controllers).
3. Runtime fixes that have to live inside xboxrecomp are kept as patches in `patches/xboxrecomp/` and applied
   by the build scripts; the submodule itself stays untouched (upstream PRs planned).

## Working with Claude Code

Start `claude` in the repo root: [CLAUDE.md](CLAUDE.md) explains the goal, layout, workflow and rules.
`scripts/fetch_references.sh` fetches the reference projects into `reference/` (read-only, not committed).

## References

- xboxrecomp: https://github.com/sp00nznet/xboxrecomp (MIT)
- SSX3-recomp by Koen Hendriks and Corné Dorrestijn: https://github.com/koenhendriks/SSX3-recomp – an independent,
  Windows-first xboxrecomp port of the same game (same PAL XBE), much further along. We use its findings as a
  reference (GPL-2.0-or-later; no code copied).
- SSX Tricky PC (xboxrecomp port of the predecessor): https://github.com/MatiasRiveraC/SSX-Tricky-PC
- OpenTricky: https://github.com/GiZcesi/OpenTricky (GPL-3.0-only)
- SSX3 PS2 decomp: https://github.com/ssxdecomp/ssx3 · GameCube: https://github.com/ssxdecomp/ssx3_gc
- [xemu](https://xemu.app) as the behaviour reference

## Legal

Unofficial fan project, not affiliated with or endorsed by Electronic Arts or Microsoft. SSX and SSX 3 are
trademarks of Electronic Arts. The repository holds only original code, tooling and notes.

License of our own files: still to be decided (xboxrecomp: MIT; code taken from OpenTricky or
koenhendriks/SSX3-recomp would bring GPL obligations, so none is taken until this is decided).
