# TODO – SSX3-Recomp bring-up

Order follows `CLAUDE.md` / `xboxrecomp/docs/GETTING_STARTED.md`. Details and findings → `docs/notes.md`.

## Phase 1 – Pipeline (static recompilation)
- [x] 0. `scripts/setup.sh` – venv + capstone 5.0.7 present
- [x] 1. XBE present (`game_files/default.xbe`, already extracted)
- [x] 2. `run_pipeline.sh parse` → `game_files/default_analysis.json` (the name is mandatory); values in `docs/notes.md`
- [x] 2b. Fill constants into `src/main.c` (entry point, XBE path, game folder)
- [x] 3. `run_pipeline.sh disasm` – 17,017 functions (16,791 in .text), 131,021 xrefs, ~9.5 min
- [x] 4. `run_pipeline.sh funcid` – 9,161 GAME (5,217 vtable), 13 CRT, 0 RenderWare, 11,214 unknown
- [x] 4.5 `run_pipeline.sh abi` – 17,017 entries, 2,666 thiscall
- [x] 4.6 Ghidra names: `scripts/ghidra.sh` (Linux wrapper) + pipeline step `names` (`merge_names.py --apply`, automatically after disasm) → 143 names
- [x] 5. `run_pipeline.sh recomp` – 17,037/17,037 translated, 0 errors, 1.37 M lines of C in 69 files (50 MB, largest 1.7 MB), 233 unresolved call targets stubbed, ~2 min
- [x] 6. `scripts/doctor.sh` – 0 ICALL targets (static, without a runtime log), 40 unimplemented instructions
- [x] 6b. Unresolved call targets 231 → 8: cause was `--text-only` (XDK sections D3D/D3DX/XGRPH/DSOUND/XPP were missing) → `--extra-sections`
- [ ] 6b'. Review the remaining 8 stubs
- [ ] 6c. Check unimplemented instructions: aaa/arpl/insb/outsd/verw/sldt/int1 = probably data as code (wrong functions?); wbinvd/cli/sti/hlt/sgdt/ljmp = XDK/kernel-level, no-op OK

## Phase 2 – Build (Linux, SDL2 + OpenGL)
- [x] Linux build deps already present (cmake, gcc, clang, ninja, SDL2 2.32, GL, OpenSSL 3.6, ffmpeg)
- [x] `src/main.c` / `CMakeLists.txt` cross-platform: shared `boot_and_run()`; Windows = WinMain + VEH + dbghelp,
      POSIX = `main()` + sigaction (SIGSEGV/SIGBUS, `xbox_PosixMmioFault`) + `xbox_HostWindowMain`, dladdr/backtrace in the crash report
      (pattern from `xboxrecomp/docs/runtime/README.md` "POSIX hosts"). CMake: Win libs/`/bigobj` Windows only, MinGW `-Wa,-mbig-obj`,
      POSIX threads/dl/m + ENABLE_EXPORTS, GCC/Clang `-fno-strict-aliasing`
- [x] Windows build cross-checked: MinGW cross build on Linux, runs under Proton (MSVC still untested)
- [x] `scripts/build.sh` – compiles (GCC 16.2, ~1:40 min with `JOBS=8`, ~0.4 GB RAM per compiler); run: `scripts/run.sh`

## Phase 3 – First start / debugging loop
- [x] First start: 5 ICALL targets + 3 thread contexts → 8 seeds (`config/seed_functions.json`), regenerated → no more ICALL errors
- [x] Crash in `SetEvent` (KEVENTs built by the title) → `RECOMP_TITLE_KEVENTS=1` (default in `scripts/run.sh`)
- [x] Crash in D3D pushbuffer code: device pointer NULL because `Direct3D_CreateDevice` failed (truncated D3D functions) → XDK sections
- [x] Hang in `D3D_BlockOnFence` → `xbox_Nv2aMirrorFence(0x0024B0D8, 0x2C, 0x30)` in `src/main.c`
- [x] Hang in `BlockUntilVerticalBlank` → `RECOMP_VBLANK=1` (default in `scripts/run.sh`)
- [x] 3 new ICALL targets + 1 thread context seeded (12 seeds) → NULL loop gone, boot loads ~30 asset files
- [x] DirectSound/APU: `RECOMP_AC97_READY=1` (default). Windows/Proton worked right away; Linux needed xboxrecomp patch 0001
      (x86-64 MMIO decoder for ucontext: APU/AC'97/OHCI) + case-insensitive paths → both platforms equally far
- [x] Windows build via MinGW cross compile (`scripts/build_win.sh`) + Proton start (`scripts/run_win.sh`)
- [x] DSP handshake: runs automatically (runtime "DSP doorbell ack: auto")
- [x] Graphics, first image: map the memory layout before the SDL/GL window (`boot_memory()`), disabled vertex attributes
      in the executor (runtime patch) → EA BIG, THX, intro video visible (CPU rasteriser + `RECOMP_FB_WINDOW`)
- [x] Boot logos made skippable: `SSX3_SKIP_LOGOS=1` (default), `SSX3_SKIP_INTRO=1`
- [x] USB controller: call `xbox_OhciInit()`; XPP callbacks 0x00281A82, 0x0028101D (runtime) + 4 seeded statically from the
      XPP pointer tables (19 seeds)
- [x] 19 seeds → key presses work: skip intro, title screen, main menu, character select reached
- [x] Linux performance: raster pool on POSIX + wall-clock fps (CPU 4.4 → 15 fps), then the Vulkan back end:
      **60 fps** title/menu, 55–57 fps character select (`SSX3_RENDERER=vulkan`, default; `=cpu` as reference)
- [x] **Vulkan M3:** interface extension (combiner state, texture stages 1–3, specular/fog) + combiner uber-shader;
      rider model solid again. Exact texture hash sizes, vertex cache, pre-decoded vertex programs
- [x] Race started and in-game graphics checked (Vulkan matches the CPU reference): race **55–60 fps**, pre-race
      screen ~40 fps
- [x] Rider vanishing at the Snow Jam lift station (stride-0 vertex attribute), dark squares on the snow / missing
      rider shadow (one GPU image per surface, clip as scissor) – see notes.md 2026-10-19
- [ ] Occasional rider-shadow glitches (user report) and a lighter rectangle under the rider at ~0:21 on Snow Jam –
      compare with xemu (Xbox reference) before changing anything
- [ ] **Pad recorder skill** (its own Claude skill, like `.claude/skills/release`): record a real pad while the user
      plays, replay it in test runs (by the game's step counter, app+0x20) – for sections the no-input scripted run
      cannot reach
- [ ] Pre-race screen / busy race scenes below 60 (race dips to 37–46 fps at 700–840 draws): NV2A thread is CPU-bound in the vertex program interpreter →
      parallelise transforms over the raster pool, or (long term, needed for >60 fps) vertex programs on the GPU
- [x] Random 1–70 s boot (lost vblanks) and empty title screen after the intro fixed (runtime patch)
- [ ] Crash right after the intro/attract video opens (`sub_00177F80`, VA 0xE0220705), intermittent.
      `reference/SSX3-recomp-koen/CLAUDE.md`: `sub_00177F80` = EA STRM "next chunk"; their ring corruption came from
      critical sections being preemption points (fix: `RECOMP_CS_KEEP_CPU`) and unfair guest-CPU handover (`RECOMP_FAIR_YIELD`)
- [x] **Above 60 fps:** frame interpolation in the Vulkan back end (presenter thread, game-clock timing):
      120 fps steady with `SSX3_FPS_LIMIT=120`, ~270 at a 270 Hz display (`docs/frame-interpolation.md`)
- [ ] Interpolation follow-ups: blend animated UVs/colours, cut detection, latency option
      (`SSX3_INTERP=0` today), verify on the Steam Deck (90 Hz OLED / 60 Hz LCD)
- [ ] **True uncapped frame rate (game logic, not interpolation).** Today the simulation is a fixed 60 Hz step
      (1/60 is a constant in ~50 code sites), so everything above 60 is interpolated and costs ~1 game frame +
      cushion (~24 ms) of display latency. Goal: run the game logic itself at the display rate / uncapped, so input
      is read and answered every shown frame. Steps: find every 1/60 site and the step counter (app+0x20) and the
      30 Hz flag (app+0x38, koen); decide variable delta vs. a scaled fixed step (N sub-steps per 60 Hz tick) per
      system (physics, animation, particles, camera, audio timing, replays); check physics stability and trick
      timing at 120/144/240; the guest's 60 Hz XAPI frame timer and vblank wait must follow the new rate; keep
      interpolation as the fallback. Prove each site in the disassembly first; check koen's 60 fps work.
- [ ] VBlank rate: the runtime ticks ~50 Hz by accident; derive it from the display mode (PAL 50 / PAL-60 / NTSC),
      check which mode SSX 3 (PAL) sets (the game's frame pacing uses its own 60 Hz XAPI timer, not vblank)
- [ ] Vulkan M3 leftovers: cube maps (face 0 only), bump-env/dot-product stage modes, palettised textures
- [x] Vulkan internal resolution 1x–5x (`SSX3_RES_SCALE`, F10) + two frames in flight
- [ ] Vulkan M4: verify render-to-texture, AA, per-triangle culling
- [x] **Port the Vulkan renderer to Windows** (2026-10-26): native Win32 window + `VK_KHR_win32_surface`
      (`src/render/vk_window_win32.c`), keys/F10/F11/Alt+Enter through the message loop, monitors via
      EnumDisplayMonitors, interpolation presenter on Windows, `run.ps1` / `run_win.sh` default `vulkan`, CPU fallback
      without a Vulkan driver. Tested under Proton only (notes.md 2026-10-26). Target order:
  - [ ] **Windows 10 and 11** (x86-64) – works under Proton; still to test on real hardware (window placement,
        high-DPI, EDID monitor names, alt-tab out of full screen, NVIDIA/AMD/Intel drivers).
  - [ ] **Windows 8 / 8.1** – later. Vulkan drivers exist; check the imports: UCRT (`api-ms-win-crt-*`, needs
        KB2999226 on 8.x), `XAudio2_8` and `xinput1_4` are fine from 8 on.
  - [ ] **Windows 7** – later. `XAudio2_8`/`xinput1_4` are Windows 8+ → load XAudio2_7 (DirectX redist) or use
        WASAPI/DirectSound, `xinput9_1_0`/`xinput1_3` at run time; UCRT via KB2999226 or build against msvcrt; check
        Media Foundation (`MFPlat`/`MFReadWrite`, movies) and `D3DCOMPILER_47` (CPU path's D3D11); NVIDIA/AMD still
        ship Vulkan drivers for 7 on older GPU generations.
  - [ ] **Windows XP** – maybe, a stretch: no Vulkan there (and 64-bit only on XP x64), so it would need a
        D3D9 or OpenGL back end, a 32-bit or XP-x64 build and an older toolchain/CRT – evaluate after 7 works.
- [x] **Windows/Proton: crash when a race loads** (fixed 2026-10-26: `RECOMP_WIN_PLACEHOLDER=1` default in WinMain). Release v0.1.0 exe under GE-Proton: `tiled aperture at 0xF0000000
      failed (error 487)` and RAM mirrors 26/27 (0x78000000/0x7C000000) fail to map; menus work, then entering a race
      writes to Xbox VA 0xF0610000 → access violation (guest stack 0x001F0386 ← 0x000E2688). Check on real Windows;
      find what Wine has reserved there (koen's port is Windows-first – see how it maps the aperture).
      Still there on 2026-10-26 with both renderers (same fault, 0xF0610000 / 0xF0710000); blocks the Windows race test.
- [ ] Interpolation at high internal resolution: the presenter re-renders at the display's refresh and can take
      the GPU from the game (5x, 270 Hz: game ~25 fps; `SSX3_FPS_LIMIT=120` keeps 60) – budget the presenter
- [ ] Downscaling 4x/5x to a small window is one linear blit (aliasing); a box/mip downsample would give real SSAA
- [x] Widescreen 16:9 / 16:10 / 21:9 / auto (`SSX3_ASPECT`): camera hook + `R.wide`; HUD 4:3 middle, menus pillarboxed
- [x] Display modes: windowed / borderless / exclusive fullscreen (`SSX3_WINDOW`, start-up menu), Alt+Enter / F11 toggle
- [x] Monitor choice: the desktop's main monitor by default, `SSX3_MONITOR` + start-up menu question
- [ ] Display modes: check exclusive fullscreen on the Deck (gamescope) and Windows; windowed placement on Wayland
      is the compositor's choice (KWin: usually the active screen)
- [ ] Widescreen: check split screen, in-game cut-scenes/replays, follow window resizes (`auto` reads the desktop once)
- [ ] Fence: replace the mirror with `nv2a_pb_set_semaphore_target` once desyncs/graphics glitches show up
- [ ] `t:\$u\contentmeta.xbx` is missing at boot – check whether it matters
- [ ] Offer xboxrecomp patch 0001 as an upstream PR (sp00nznet/xboxrecomp)
- [ ] Milestones: EA logo → title screen → menu → race
- [ ] Compare OpenTricky/SSX-Tricky-PC changes to xboxrecomp (their NV2A→D3D11 renderer as a reference for M3)
- [ ] Go through `reference/SSX3-recomp-koen` (same game, same XBE) for fixes we still lack: unresolved stubs into
      function middles (Options-menu crash, Equip Gear), media check `sub_00183EBB`, (x87 `exp`/`fmod` ebx bug: done),
      30 Hz flag app+0x38, vertex-colour clamping, stencil, point-size clamp, GPU vertex programs, widescreen camera
- [ ] xemu reference tooling for Linux (xemu 0.8.136 flatpak is installed): boot/snapshot/press/capture over QMP,
      plus a Claude skill – modelled on their Windows-only `tools\xemu.ps1` skill, our own code

## Features
- [ ] **Language switcher.** Our base is the PAL/EU build, which ships English, French, German and Spanish
      (`data/locale/*{amer,fren,grmn,span}.loc`; the boot currently loads `cmnamer.loc` = English). Find out how the game
      picks the language (probably the Xbox dashboard language via `XGetLanguage` / `ExQueryNonVolatileSetting` – verify in the
      disassembly), then expose it as a host option (e.g. `SSX3_LANGUAGE=en|fr|de|es`, later in a launcher/settings menu).

- [x] **Controllers (XInput / Xbox-style).** `src/input/ssx3_pad.c`: SDL2 GameController on POSIX, XInput on Windows,
      hotplug, one pad per Xbox port, LB/RB = White/Black, `SSX3_PAD_TRACE=1`, `scripts/test_pad.sh` (virtual pad).
- [ ] **PlayStation controllers** (DualShock 4, DualSense): already work positionally through SDL (Cross = A, Circle = B,
      Square = X, Triangle = Y) – verify on hardware; extra binds for touchpad click/Share; light bar; rumble hints for
      the HIDAPI PS4/PS5 drivers; PlayStation button glyphs in the HUD (game draws Xbox prompts).
- [ ] **Switch controllers** (Pro Controller, Joy-Con pair, 8BitDo in Switch/D-input mode): SDL2 maps Nintendo pads
      *by label* by default (`SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS`), so A/B and X/Y sit swapped compared to an
      Xbox pad – decide positional vs. label (an option), single Joy-Con, test each 8BitDo mode.
- [ ] **Rebinding.** The binding table `s_bind` in `ssx3_pad.c` is data already: load it from a config file
      (e.g. `config/input.ini`, `white = lb`), per port, then a menu in a launcher/settings screen; deadzone and trigger
      threshold options. First check whether the game's own `config\input*.map` allows rebinding in-game.
- [x] **Rumble.** Runtime patch forwards the game's output reports (SSX 3: HID SET_REPORT on the control pipe) to
      `xbox_InputSetState()`; SDL rumble held until the game changes it. Seen in a race (`SSX3_PAD_TRACE=1`).
- [ ] Rumble on a real pad: feel-check with the 8BitDo / an Xbox pad (strength, whether "low" = big motor feels right).
- [x] **Split screen**: `RECOMP_USB_PADS=2` with two pads works (menus, Snow Jam race, results; 2026-10-19).
- [ ] Split screen: make 2+ emulated pads the default (today Multi Play stays hidden unless `RECOMP_USB_PADS=2`
      is set), and reach the game's 30 fps cap (measured ~27).
- [ ] Widescreen: the pause menu panel sits off-centre in 16:9 (race visible in a strip on the left).

## LAN multiplayer (new feature, plan: [lan-multiplayer.md](lan-multiplayer.md))

- [x] Phase 0 spikes: rider manager / 6 race slots, puppet hook on AI riders, race start/finish/results,
      spawning riders in free roam + guest memory, frontend menu entry, save import (*Load game*), 12-rider perf
      (open from them: popup label for entry 1, Multi Play cursor with one pad, gear categories and gear stats)
- [x] Net core (`src/net/`, ENet): protocol + versioning, LAN discovery, direct IP, clock sync, headless test clients
      (`ssx3_lanbot selftest`); open: a real build/XBE hash as the game id, Windows-native token RNG
- [ ] Lobby: LAN entry in Multi Play, host/join/browse, name + character + leveled-rider import, ready check, kick,
      text chat, host rules (leveled vs. level 1, Bragging Rights, AI fill-in, collisions, max players)
- [ ] Race up to 6 players: remote players in the AI slots, AI fill-in, synced countdown, results, session scoreboard
      (done, env-driven: 2-instance race with puppets, synced start, own finish times in the results;
      character, name and input-driven animation done; open: gear on the puppet, start-list names,
      host-simulated AI, more than 2 players)
- [ ] Name tags over riders (colours, distance fade, toggle, off-screen arrows)
- [ ] Conquer the Mountain free roam, 12 players from the start; late join + reconnect
- [ ] Collision toggle (ghost / bumping with knockdown events)
- [ ] Spectators (first cut done: `SSX3_LAN_SPECTATE=1` watches one player; open: Off Limits whiteout desync,
      switching the watched player, joining a running race)
- [ ] Direct IP / internet hardening (loss, jitter, timeouts; NAT via port forwarding or VPN)
- [ ] Freestyle events: 6 players, then 12
- [ ] 12-player races (raise the engine's 6-rider limit)

## Open
- Which XBE is the base? `default.xbe` = original + region/media patch + 1-byte code patch (see notes.md).
