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
- [ ] Pre-race screen / busy race scenes below 60: NV2A thread is CPU-bound in the vertex program interpreter →
      parallelise transforms over the raster pool, or (long term, needed for >60 fps) vertex programs on the GPU
- [x] Random 1–70 s boot (lost vblanks) and empty title screen after the intro fixed (runtime patch)
- [ ] Crash right after the intro/attract video opens (`sub_00177F80`, VA 0xE0220705), intermittent
- [ ] VBlank rate: the runtime ticks ~50 Hz by accident; derive it from the display mode (PAL 50 / PAL-60 / NTSC),
      check which mode SSX 3 (PAL) sets; prerequisite for the unlocked/high-fps work
- [ ] Vulkan M3 leftovers: cube maps (face 0 only), bump-env/dot-product stage modes, palettised textures
- [ ] Vulkan M4: verify render-to-texture, AA/resolution, per-triangle culling, frames in flight, Windows (Win32 surface)
- [ ] Fence: replace the mirror with `nv2a_pb_set_semaphore_target` once desyncs/graphics glitches show up
- [ ] `t:\$u\contentmeta.xbx` is missing at boot – check whether it matters
- [ ] Offer xboxrecomp patch 0001 as an upstream PR (sp00nznet/xboxrecomp)
- [ ] Milestones: EA logo → title screen → menu → race
- [ ] Compare OpenTricky/SSX-Tricky-PC changes to xboxrecomp (their NV2A→D3D11 renderer as a reference for M3)

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
- [ ] **Rumble.** `xbox_InputSetState()` is implemented, but the emulated USB pad (`xboxrecomp/src/usb/ohci.c`) only
      counts the game's OUT reports on endpoint 2 instead of forwarding them – needs a runtime patch.
- [ ] **Split screen**: test `RECOMP_USB_PADS=2` with two pads; maybe make it the default.

## Open
- Which XBE is the base? `default.xbe` = original + region/media patch + 1-byte code patch (see notes.md).
