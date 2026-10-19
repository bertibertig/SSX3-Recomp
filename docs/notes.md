# SSX3-Recomp work log

## Game version
| Field | Value |
|---|---|
| Region / version | PAL/EU (build path `d:\ssx\code\xbox_eu\obj\releasedvd\ssx.exe`, original region flag 0x4 = PAL), build 2003-09-19 |
| Languages | English, French, German, Spanish (`data/locale/*{amer,fren,grmn,span}.loc`) |
| default.xbe SHA-1 | `409141774025fce02ab4743d1e669d35bb1d190e` (patched, see below); original `default.xbe_orig`: `ba876cc11ad38650eac9831b1c7f8c4dbf7c5ba9` |
| Title ID | 0x4541003C ("EA" 0x3C) |
| Base address | 0x00010000, image size 3,864,448 |
| Entry point | 0x00181AD2 (filled into `src/main.c`); kernel thunk 0x00281C40; TLS index 0x003A1C00 |
| .text size / functions | 2,201,308 B / 17,017 functions (16,791 .text, 162 D3D, 39 DSOUND, 13 XPP, 10 XGRPH, 2 D3DX) |
| XDK version | 5558 (XAPILIB, D3D8, D3DX8, XGRAPHC, XBOXKRNL, LIBC, DSOUND – all 1.0.5558) |
| Kernel imports | 159 |

## Sections (xbe_parser)
| Name | VA | VSize | Flags |
|---|---|---|---|
| .text | 0x00011000 | 2,201,308 | X |
| D3D | 0x0022A6E0 | 143,872 | W X |
| D3DX | 0x0024D8E0 | 12,920 | W X |
| XGRPH | 0x00250B60 | 10,356 | W X |
| DSOUND | 0x002533E0 | 156,572 | W X |
| XPP | 0x00279780 | 33,960 | W X |
| .rdata | 0x00281C40 | 497,840 | RO |
| .data | 0x002FB500 | 748,024 (raw 300,136 → BSS part) | W |
| DOLBY | 0x003B1F00 | 29,056 | |
| $$XTIMAGE / ABRTFONT / .XTLID … | from 0x003B9080 | | resources |

## XBE variants in game_files/
All three are 3,428,352 bytes; differences against `default.xbe_orig`:
- `default.xbe`: certificate media flags 0x2 → 0xC00001FF, region 0x4 → 0x7 (region/media-free) **plus 1 code byte**
  at file offset 0x1740F3 = VA 0x001840F3: `test eax,eax; jge +5` (0x7D) → `jmp +5` (0xEB) directly after a call
  in the XAPI process initialisation (after `HalRegisterShutdownNotification`): a result of `sub_00183EBB` < 0
  originally led into an error path (0x001840F5 → 0x0018411B), now it always continues to 0x001840FA. Typical HDD/softmod patch.
- `default_ffp.xbe`: like `default.xbe` + 12 bytes in the D3D section (VA ≈ 0x0022B5BB / 0x0022CFE2), probably a
  flicker-filter/progressive patch – irrelevant for the port, since D3D is replaced.
- The pipeline runs on `default.xbe`. The code patch is harmless for the recomp/probably even helpful; if
  behaviour differs, cross-check against `default.xbe_orig`.

## Checklist
- [x] `scripts/setup.sh` run (venv, capstone 5.0.7) – Linux deps still open
- [x] ISO extracted, XBE present
- [x] `parse` run, values filled in above + in `src/main.c`
- [x] disasm / func_id / **abi_analysis** / recomp successful
- [x] `scripts/doctor.sh` read
- [x] First build compiles (Linux, GCC 16.2, 1:38 min with 8 jobs, `build/ssx3_recomp` 20 MB)
- [ ] First start: up to EA logo / title screen / menu / race
- [ ] Check renderer (NV2A → D3D11 or OpenGL)
- [ ] Compare with OpenTricky's changes to xboxrecomp

## Open questions
- Similarity SSX3 engine ↔ SSX Tricky: which parts (archive/model formats, boot, renderer) can be reused?
- Linux build (OpenGL) vs. Windows build (D3D11): which route first?

## Log
_(date – what was done, what was found, next step)_

- 2026-10-17 – Repo created, xboxrecomp v0.14.0 as submodule, scripts, reference repos fetched.
- 2026-10-17 – parse: values filled in above. `run_pipeline.sh` wrote `ssx3_analysis.json`, but disasm expects
  `<xbe-stem>_analysis.json` next to the XBE → script switched to `default_analysis.json`. TODO list: `docs/TODO.md`.
- 2026-10-17 – Pipeline complete (logs in `logs/`, gitignored):
  - disasm 570 s: 17,017 functions, 131,021 xrefs (37,558 call, 558 kernel_call). Top kernel: NtClose, RtlLeaveCriticalSection.
  - func_id: 9,161 GAME (8,588 vtable), 13 CRT, **0 RenderWare** (SSX3 uses its own EA engine), 11,214 unknown.
  - abi_analysis: 17,017 entries, 2,666 thiscall.
  - recomp (`--split 250`): 17,037/17,037, 0 errors, 1,374,816 lines, 69 files, 233 unresolved call targets, 40 unimpl. insns.
    No setjmp/longjmp found; SEH helpers `__SEH_prolog` 0x00213F68 / `__SEH_epilog` 0x00213FA3.
  - doctor: only item "40 unimplemented instructions" (see TODO 6c).
  - Ghidra names (4.6): `scripts/ghidra.sh` replaces the Windows-only `run_ghidra.sh` (analyzeHeadless instead of .bat, venv python instead of `py -3`,
    no cygpath). Ghidra 12.1.2 (`/opt/ghidra`) only ships Jython as an extension zip → the script unpacks it once into
    `~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/Jython` (the xboxrecomp scripts are `@runtime Jython`). Heap 8G.
    Run 2026-10-18: ~5 min, Ghidra 9,827 functions / 78,789 symbols; `merge`: 388 names (143 FidDb/CRT, 245 string labels),
    143 hit recompiler function starts. Not yet applied with `--apply`.
    Triage without GUI: `scripts/ghidra.sh classify <VA…>` (read-only, ~30 s), e.g. 0x0024B0D8 = DATA, 0x001EA662 lies in FUN_001EA590.
  - `_common.sh`: `PYTHONUNBUFFERED=1`, otherwise redirected logs stay empty for minutes.
  - Next step: phase 2 – adapt `main.c`/CMake for Linux (SDL2/GL), first build.
- 2026-10-17 – **Host cross-platform** (`src/main.c`, `CMakeLists.txt`): shared `boot_and_run()`;
  Windows: WinMain + VEH + dbghelp (unchanged); POSIX: `main()` + sigaction(SIGSEGV/SIGBUS, SA_ONSTACK) →
  `xbox_PosixMmioFault`, otherwise crash report with dladdr + backtrace; `xbox_HostWindowMain(boot_and_run)` (window with
  `RECOMP_FB_WINDOW`). Template bug: `recomp_dispatch_init` was not declared (MSVC only warns, GCC ≥14 errors).
  `_GNU_SOURCE` needed for dladdr/REG_RIP. CMake: `-fno-strict-aliasing` (GCC/Clang), ENABLE_EXPORTS, MinGW `-Wa,-mbig-obj`.
  Windows build is **untested** (no Windows here).
- 2026-10-17 – **First start** (`logs/09_run1.log`): boot gets through the memory layout (64 MB @ offset, heap 48 MB, stack
  8 MB @ 0x00780000), 159/159 kernel thunks, XAPI init (DVD auth, TDATA/UDATA/4541003c, TitleMeta.xbx, partition reads),
  `t:\$u\contentmeta.xbx` missing (ERROR_PATH_NOT_FOUND – not critical?).
  - 5 unresolved ICALLs: 0x0027AC53 (XPP), 0x0025AF68/73, 0x0025EA0D/18 (DSOUND) = C++ static initialisers
    (`mov [global], imm; ret`), called via `_initterm`. + 3 thread contexts (0x0017ACE0, 0x00181A5E, 0x0021C1B5).
    → `tools.seed_from_log` → `config/seed_functions.json` (8 seeds, all verified in the disassembly).
    `run_pipeline.sh` now passes the seeds to disasm automatically.
  - Crash in native `SetEvent`/`pthread_mutex_lock`: the title builds its KEVENTs itself (header written directly, e.g.
    0x00F7F0D8 on the main stack, thread 0x0021C1B5 sets it). Without a shadow, the guest VA goes to Win32 as a HANDLE –
    on Windows SetEvent silently fails, on POSIX (`win32_compat.c`) it is dereferenced → SIGSEGV.
    **Fix: `RECOMP_TITLE_KEVENTS=1`** (runtime opt-in). (Upstream candidate: win32_compat should reject invalid handles.)
- 2026-10-17 – **Second start** with `RECOMP_TITLE_KEVENTS=1` (`logs/11_run2_kevents.log`, still without seeds): gets as far as
  4 threads/3 NtCreateEvents, then crash in the **XDK D3D pushbuffer code** `sub_00234B30` (D3D section, writes NV2A methods
  0x4A310/0x41D70/0x41D90 – fence/kickoff). Put pointer from `*(*(0x0024B0D8))` = 0xF6808009 (unaligned, outside any
  aperture). 0x0024B0D8 lies in the BSS part of the D3D section → set during D3D device init. Callers: game code
  0x001EA662 → 0x0022E8CF → 0x00234F00 → 0x00234C0F → 0x00234B30.
  SSX3 links the XDK D3D statically → the right route is the **pushbuffer executor** (`RECOMP_PB_SCAN`/`RECOMP_PB_EXEC`,
  `xboxrecomp/docs/technical/pushbuffer-executor.md`), not the D3D8 HLE layer. Next step: find out who initialises
  0x0024B0D8 or the pushbuffer put (Ghidra/REA xrefs), and start again with `RECOMP_PB_SCAN=1`.
- 2026-10-18 – **D3D crash analysed** (Ghidra project from `scripts/ghidra.sh`, new queries `scripts/ghidra.sh xrefs|decomp`,
  gdb breakpoints on the generated `sub_*` symbols; `RECOMP_KERNEL_LOG_BUDGET=2000` for the full kernel trace):
  - In the crash `esi=0`: `sub_00234B30` first loads `esi = [0x0024B0D8]` → **device pointer NULL**; 0xF6808009 is just
    the content of Xbox VA 0. The only writer of 0x0024B0D8 is `FUN_0023F720` (= `Direct3D_CreateDevice`, static
    device at 0x0024B0E0; resets the pointer to 0 on failure). Only caller: game code 0x001AE1F4 in
    `FUN_001AE160` – the return value is ignored, the game keeps drawing.
  - Device layout (from `FUN_00234680`, pushbuffer init): +0x00 put, +0x04 limit, +0x24 base, +0x28 end, +0x2C ref/fence,
    +0x30 fence page, +0x44 fence ring; pushbuffer 0x80000 B (`DAT_0024D8D4`), kickoff 0x8000 B (`DAT_0024D8D0`).
    `FUN_00184212` = XPhysicalAlloc-like (attr < 0 → `MmAllocateContiguousMemoryEx` via `FUN_0018915d`).
  - gdb: `sub_0023F720` → `sub_0023A840` (device init) → immediately `sub_0023AD10` (cleanup); `sub_0023A2A0` is never reached,
    and there are only 2 contiguous allocations (4 KB, freed again, and 45 MB by the game) – the pushbuffer is never created.
  - **Cause:** `--text-only` – the XDK sections D3D/D3DX/XGRPH/DSOUND/XPP are not linearly swept. The recompiler
    only knows `sub_0023A840` up to 0x0023A86E; the rest is an empty stub (`recomp_stubs_unresolved.c`: "0x0023A86E: not
    detected") → init returns garbage → CreateDevice fails. `0x00234620`/`0x00234680` were missing entirely too
    (no function between 0x002344E0 and 0x00234720). Many of the 231 stubs lie at 0x0023…/0x0025….
  - **Fix:** `run_pipeline.sh` passes disasm `--extra-sections D3D,D3DX,XGRPH,DSOUND,XPP` (not DOLBY: DSP code).
    At the same time step 4.6: new pipeline step `names` (`merge_names.py --apply`) runs automatically after every disasm.
- 2026-10-18 – **After the XDK section fix** (`logs/16_pipeline_xdk_names.log`): 18,599 functions (D3D 712, D3DX 87,
  DSOUND 789, XGRPH 49, XPP 240; before 162/2/39/10/13), unresolved call targets 231 → **8**, 143 Ghidra names applied.
  No more crash: `Direct3D_CreateDevice` runs completely (pushbuffer 2 MB @ 0x80002000, framebuffer 2400+1200 KB,
  `KeConnectInterrupt` vector 3 → ISR 0x0023B290).
  - Hang 1: `D3D_BlockOnFence` (0x00234BE0) waits until the GPU fence word `*(dev+0x30)` reaches the counter `dev+0x2C` –
    nobody executes the pushbuffer. **Fix:** `xbox_Nv2aMirrorFence(0x0024B0D8, 0x2C, 0x30)` in `src/main.c`
    ("always caught up"; only valid without `RECOMP_PB_EXEC`, otherwise `nv2a_pb_set_semaphore_target`).
  - Hang 2: `D3DDevice_BlockUntilVerticalBlank` (0x0022DF20) waits on event `dev+0x1DAC` (0x0024CE8C), which the
    VBlank DPC sets. **Fix:** `RECOMP_VBLANK=1` (runtime 60 Hz VBlank; ISR "claimed it") – default in `scripts/run.sh`.
  - After that: first game asset opened (`D:\data\ui\mapgfx.big`), >100,000 ICALLs. Then 3 new ICALL failures: 0x00279960,
    0x0027A16F (XPP, USB host init, pool tag 'USBH'), 0x002000E0 (.text, Ghidra: function start; disasm: alias in
    sub_00200041) + thread context 0x001AE3B0 → seeds (now 12). Follow-up hang: infinite loop over a NULL function pointer
    (193 M ICALLs to target 0) – probably because the missing initialisers never ran.
  - Tools: `scripts/ghidra.sh xrefs|decomp` (own scripts in `scripts/ghidra/`), gdb with a Python timer
    (stop the process after N s, `thread apply all bt`), `RECOMP_KERNEL_LOG_BUDGET`, `RECOMP_WATCHDOG_SECS`.
- 2026-10-18 – **With 12 seeds** (`logs/25_run8.log`): NULL loop gone. Boot now loads `ui\mapgfx.big`, `audio\{music,music2,
  audio,speech}.big`, `scripts\scdat.big`, all `char\*txx.big` + `mdlxbx.big`, `config\input*.map`, `fonts\{fe,hud}font.ffn`,
  `char\boltxbx.dat`, `be\rwrdxbx.dat`, `be\charDB.dbl`, `locale\cmnamer.loc` – then crash in DirectSound:
  - `IDirectSound_CreateSoundBuffer` (0x00258FFD) → 0x00258A38 → 0x00258830 → buffer voice ctor `FUN_00260DCD` → `AddRef` on
    `*(*(buffer+8)+0xC)` = **0xFFFFFFFF** → DirectSound object never fully initialised (`ebx=0x8007000E` is just the
    constant from `(p ? S_OK : E_OUTOFMEMORY)`). Cause according to `xboxrecomp/docs/pipeline/06-debugging.md` ("Runs, Never Draws
    a Frame"): XDK DirectSound checks the AC'97 codec; without `RECOMP_AC97_READY=1` the init fails.
  - With `RECOMP_AC97_READY=1` (`logs/26_run9_ac97.log`): emulated APU starts (SDL2 audio), APU aperture 0xFE800000..0xFE830000
    is trapped for MMIO, codec reports ready. Crash then in `sub_00263DB4` when **reading APU register 0xFE801100**:
    the trap fires, but `xbox_PosixMmioFault` **only has a decoder on arm64** – on Linux x86-64 it declines → SIGSEGV.
    The x86-64 decoder (`xboxrecomp/src/platform/mmio_decode.h`) and `apu_hook_handle_mmio` are restricted to `_WIN32`
    (they work on `CONTEXT`). On Windows this step should work. → Gap in the runtime, not in the game.
    In addition the runtime warns: "AC97: bus-master write trap not available on this host; a channel reset will spin".
  - Expected next (comment in xbox_memory_layout.c): DirectSound hands the audio DSP a command block and waits
    until it is cleared; the DSPs are stubs → possibly `RECOMP_DSP_ACK=<va>`.
- 2026-10-18 – **Windows build under Proton** (`scripts/build_win.sh` = MinGW cross build with xboxrecomp's
  `cmake/mingw-w64-x86_64.cmake`, `scripts/run_win.sh` = umu-run/GE-Proton, working directory repo root, own prefix
  `~/.local/share/ssx3-recomp-proton`). First a build error: Ghidra names `fpclass`/`chgsign` collide with MinGW `math.h`
  (MSVC "old names") → lifter reserved list extended. With `RECOMP_AC97_READY=1` (`logs/30_win_run1.log`): APU on XAudio2,
  AC'97 trap, DSP doorbell auto-acknowledged, APU interrupts "claimed", game opens `movies\eabig.mad`, `thx.mad`,
  `intro_dj.mad` – **intro video sound audible** (confirmed by user), no picture (pushbuffer is not executed).
  → Diagnosis confirmed: on Linux only the x86-64 MMIO decoder was missing.
- 2026-10-18 – **xboxrecomp patch 0001** (`patches/xboxrecomp/`, details there): x86-64 MMIO decoder for Linux ucontext
  (APU, AC'97, OHCI), case-insensitive path resolution on POSIX (`intro_dj.mad` vs. `Intro_DJ.mad`), MinGW old names in the
  lifter. Linux (`logs/35_linux_run_case.log`) now reaches the same state as Proton. `RECOMP_AC97_READY=1` is the default in
  `run.sh`/`run_win.sh`. Decoder test: `tests/mmio_decode_x64_linux.c`.
  Next big block: **graphics** – pushbuffer executor (`RECOMP_PB_EXEC=1`) + window (`RECOMP_FB_WINDOW=1`), while replacing the
  fence mirror with `nv2a_pb_set_semaphore_target`. Videos are EA MAD (software decoder in the game, ends up as a
  texture in the pushbuffer).
- 2026-10-18 – **Graphics: first image.**
  - `RECOMP_FB_WINDOW` (SDL/GL) initialised the graphics driver before the memory layout → Mesa occupied exactly the address
    of the contiguous window (guest offset + 0x80000000) → `failed (error 8)`, crash in `sub_00184BEE`. Fix in `src/main.c`:
    `boot_memory()` (load XBE + layout) runs before `xbox_HostWindowMain`, the rest (`boot_and_run`) on the guest thread.
  - `RECOMP_PB_EXEC=1`: 264 draws, but all triangles "degenerate/NaN". `RECOMP_VSH_TRACE`/`RECOMP_VSH_DUMP`: the
    FMV/2D program (`dp4` against v8..v15) reads its matrix from **disabled vertex attributes**, set via
    `SET_VERTEX_DATA4F_M` (0x1A80..0x1AFC); the executor returned (0,0,0,1) for disabled attributes → every vertex
    (640, 0, 16777215, 1). Fixed in the runtime patch (nv2a_pb_exec.c). After that: EA SPORTS BIG, THX and the intro video
    correct in the window and in `RECOMP_FB_DUMP` images. Video frame = texture 0x823BCA00, 640x480, format 0x24 linear.
  - Fence mirror stays for now (works with the executor in segment-scan mode).
- 2026-10-18 – **Input.** `xbox_OhciInit()` was never called (the template doesn't call it) → no USB controller →
  `RECOMP_KEYBOARD`/`RECOMP_PAD_LIVE` had no effect. Call added in `boot_and_run`. After that, enumeration up to
  `SET_CONFIGURATION(1)`, then `[ICALL] Failed to resolve VA 0x00281A82` (XID class driver callback in XPP, follows
  `ret 8`) → seed (now 14). Key mapping (xboxrecomp `src/input/keyboard.c`): Enter=Start, Backspace=Back,
  Z/X/C/V=A/B/X/Y, arrows=D-pad, WASD=left stick, IJKL=right stick, 1/3=triggers, Q/E=White/Black.
- 2026-10-18 – **Boot videos.** `FUN_00080730`: `DAT_003083B8` = bitmask of the boot videos (bit0 EA BIG, bit1 THX, bit2
  intro; .data init 7), skip flag `this+0xB5A94` only set for the intro, skip keys 0x70/0x7A. Option in `src/main.c`:
  `SSX3_SKIP_LOGOS=1` (default in the start scripts) clears bit0/1 before start, `SSX3_SKIP_INTRO=1` also bit2.
  `run.sh`/`run_win.sh` now set PB_EXEC, FB_WINDOW, USB, KEYBOARD, SKIP_LOGOS by default.
- 2026-10-18 – **Front end reached.** With 19 seeds (XPP callbacks) input works: `RECOMP_PAD_LIVE` keys
  skip the intro, then title screen "Press START" → "Checking hard disk" → main menu (Single Event / Conquer The
  Mountain / Multi Play / Previews) → character select with 3D rider models. Everything rendered correctly (CPU rasteriser).
- 2026-10-18 – **Open: performance on Linux.** Title/menu: Linux **~1.5 fps**, Windows build under Proton **7.6–13 fps**
  (Proton window smaller, according to user). Same runtime + same game code → Linux-specific. Ruled out:
  USB (equally slow without `RECOMP_USB`), SDL window/vsync (same without `RECOMP_FB_WINDOW`), rasteriser (NV2A thread
  idle in `sched_yield` in 40/40 samples). gdb samples: **all threads waiting** – main loop
  (`sub_001AE3C0`) alternating between the D3D kickoff spin (`sub_00234A10`, busy bit 0x100410) and `NtWaitForSingleObjectEx`
  (`sub_00181E10` = WaitForSingleObject), workers `sub_0017ACE0`/`sub_0017BB30` in `NtWaitForSingleObjectEx`, XDK thread
  `sub_0021C1B5` in `KeWaitForMultipleObjects` (POSIX: `WaitForMultipleObjectsEx` polls with `usleep(1000)`), with a window
  additionally `D3DDevice_BlockUntilVerticalBlank` (`sub_0022DF20`). strace (`clock_nanosleep`, last ~15 s): 17,484×
  1 ms (the usleep poll), plus sleep times 4/8/14/24/28/34/44/54 ms (from guest `Sleep`/timers). Hypotheses for tomorrow:
  (1) latency of the 1 ms poll loop in `WaitForMultipleObjectsEx` – replace with a real condvar wait;
  (2) time base/clock on Linux (KeQueryPerformanceCounter/-Frequency, KeQuerySystemTime, timer DPCs with `Sleep(10)` in the
  timer thread) → game sleeps too long; (3) kickoff spin waits on the busy bit, which the poll thread only clears every 256 words.
  Tools: `RECOMP_KERNEL_LOG_BUDGET`, gdb with a SIGSTOP timer (scripts in the session), `strace -f -tt -T`
  (only as the launching process, `ptrace_scope=1`).
- 2026-10-18 – **Performance, part 1.** With `perf` (`perf record -e cycles:u -g --user-callchains`, paranoid=2):
  rasteriser-bound. The NV2A thread renders (`nv2a_rc_eval`, `sample_tex`, `read_rgb`, `raster_xf_triangle`), the main thread
  spins in the D3D kickoff `sub_00234A10`, a second D3D thread in `sub_0023B490` (both wait for the GPU). Yesterday's
  gdb SIGSTOP samples were misleading (they only hit wait states). Causes of the Linux lag:
  (1) the rasteriser thread pool was `_WIN32`-only → POSIX pool with pthreads (runtime patch); (2) the fps display used
  `clock()` = CPU time → Linux values ~3× too low (yesterday's "1.5 vs. 13 fps" was partly a measurement artefact; real 4.4 fps).
  Title screen, real: 1 thread 4.4 fps → 12 threads **15.4 fps**; `-march=x86-64-v3` only +8 % (16.7). All 12 raster threads
  evenly loaded → 60 fps needs a GPU render back end (recommendation: OpenGL first, Vulkan later).
- 2026-10-18 – Docs translated from German to English. TODO added: in-game language switcher (EN/FR/DE/ES, see `docs/TODO.md` → Features).
- 2026-10-18 – **Vulkan back end** (`src/render/`, design `docs/vulkan-backend.md`), default in `scripts/run.sh`
  (`SSX3_RENDERER=vulkan`; `=cpu` keeps the software renderer as the reference for comparison dumps).
  - M1 (device/swapchain/clear/flip) + M2 (triangles, texture stage 0, blend/depth/stencil/alpha test as dynamic state,
    one pipeline, push descriptors). The title-screen sky was black: a stale texture on stage 0 whose program is NONE
    (CPU frame trace `RECOMP_FRAME_TRACE`) → runtime patch in `backend_flush`.
  - Performance: first ~24 fps in the menu – the NV2A thread decoded every texture every frame (34 % `sample_tex`) just
    for the change hash → hash the raw guest bytes, decode only on change. After that **60 fps** title/menu,
    **55–57 fps** character select (380 draws). CPU renderer for comparison ~12 fps.
  - "Not responding" at start: events were only pumped in `flip` → window + event loop on the main thread
    (`ssx3_vk_host_main`, guest on its own thread with a 64 MB stack like `xbox_HostWindowMain`), `vkAcquireNextImageKHR`
    with a 100 ms timeout. Window title "FPS | draws", the same line every 5 s as `[VK] FPS:` in the log.
  - Open: 3D rider model semi-transparent (combiners/depth → M3), character select just under 60.
- 2026-10-18 – **Vulkan M3 + first race.** Runtime patch: `Nv2aBatchExt` (combiner state, 4 texture stages, per-vertex
  tex coords/specular/fog) for transformed batches; `nv2a.frag` became a combiner uber-shader (port of
  `nv2a_rc_eval`/`rc_stage_fetch`), state in a UBO from the frame ring. Character-select rider solid again.
  - With `SSX3_SKIP_INTRO=1` the route boot → race is: START, START, then A ×8 (peak, mode, event, rules, pre-race
    screen) – timing-sensitive, check with `SSX3_VK_DUMP`. In-game renders like the CPU reference (snow, mountains,
    riders, HUD).
  - Race first at **7–10 fps**: (1) ~22 textures re-decoded every frame (1.9 M texels) – the change hash covered
    w*h*4 bytes, for DXT textures (formats 0x0C/0x0E/0x0F) 4–8× the real size, i.e. per-frame data behind them →
    hash exactly level 0 (`texture_bytes`); also a texture outside guest RAM (hash 0) no longer re-decodes every frame.
    → 23 fps. (2) Then the vertex program interpreter was ~one full core (`nv2a_vsh_run` 24 %, `read_src` 9 %) →
    per-batch post-transform vertex cache by index + programs decoded once (runtime patch) → race **55–60 fps**,
    pre-race screen ~40. CPU renderer still renders the race (3 fps).
  - Remaining guest time is the D3D spin `sub_00234A10` waiting on the NV2A thread.
- 2026-10-18 – **Title screen empty after the intro / slow, random boot.** Two separate bugs, both runtime patch:
  - Boot (0.7 s … >70 s to the first drawn frame, random): main thread in `sub_0022C620` (D3D mode set: loop of
    `BlockUntilVerticalBlank` `sub_0022DF20` + `AvSetDisplayMode` steps) waiting on the vblank event (D3D device
    0x0024B0E0 + 0x1DAC). ISR `sub_0023B290` (vector 3) claimed every vblank, its DPC `sub_0023BC60` reads PMC_INTR_0
    and calls the vblank handler `sub_0023B490` (`KeSetEvent` on the event) only if bit 24 is still set – but the
    runtime's ack thread zeroes all interrupt status registers continuously and usually won. Fix: vblank held pending
    until D3D acks (marker bit in PCRTC_INTR_0). Boot now 0.5–0.6 s in every run. Note: the timer thread ticks
    vblank every ~20 ms (50 Hz) – by accident (Sleep(10) + 16 ms gate), not from the display mode (PAL!). Revisit
    with the unlocked-framerate work.
  - Title screen only sky after the intro was played and skipped: the intro's linear 640x480 video texture leaves its
    pitch in the texture stage; the M3 combiner path took pitch != 0 as "linear" and scaled the title textures'
    coordinates by 1/size. Runtime now passes pitch 0 for swizzled/DXT formats. My earlier tests used
    `SSX3_SKIP_INTRO=1` and missed it – test the default flow (intro + key skip) too.
  - Still open: intermittent crash right after `intro_dj.mad` opens (attract-mode video, also seen at boot):
    `sub_00177F80`, eax=0xE0220701, Xbox VA 0xE0220705 – garbage pointer, not investigated yet.
  - New: `[VK] first flip / first frame with draws at X s (flip N)` log line, Vulkan dumps named by flip number,
    `SSX3_VK_NO_RC=1` (ignore the combiner extension, for comparisons).
- 2026-10-18 – **Game controllers.** The runtime's SDL controller backend (`xboxrecomp/src/input/xinput_device.c`) never
  opened a pad: nothing calls `xbox_InputInit()`, and it only scanned once, at init (no hotplug). Black/White also sat
  on LB/RB, the opposite of its own README and xemu. Replaced in our build by `src/input/ssx3_pad.c` (CMake swaps the
  source of the `xbox_input` target, submodule untouched): host pad → modern layout (`HostPad`) → binding table
  `s_bind` → Xbox pad. SDL: rescan every 250 ms under `SDL_LockJoysticks` (the window thread pumps events, the
  USB thread reads), lowest free port per new pad, pads keep their port until unplugged. Windows: XInput through the
  same table. `main.c` calls `xbox_InputInit()` after `xbox_OhciInit()` (logs the pads found).
  - Verified: `scripts/test_pad.sh` (SDL virtual Xbox 360 pad: hotplug, map, packet numbers) passes; a real
    8BitDo Ultimate 2C (2dc8:310a, SDL type Xbox One/Series) is found at boot (`PAD: port 1 <- …`) and the game
    enumerates the USB pad as before. MinGW compile of the XInput branch clean. Not yet verified: playing with the
    pad in the menus/race (needs someone pressing buttons – use `SSX3_PAD_TRACE=1`).
  - Multiple pads on the emulated bus: `RECOMP_USB_PADS=1..4` (runtime, default 1).
- 2026-10-18 – **New reference: koenhendriks/SSX3-recomp** (`reference/SSX3-recomp-koen`, via `fetch_references.sh`).
  An independent xboxrecomp port of the same game: its CLAUDE.md lists PAL, title ID 0x4541003C, XDK 5558, built
  2003-09-19, entry 0x00181AD2 and the D3D fence mirror at 0x0024B0D8 – identical to our XBE, so its guest addresses
  apply directly (still verify). Windows/MSVC-first, playable to all peaks. License GPL-2.0-or-later: we use findings,
  not code (see CLAUDE.md → References). Its xemu skill wraps a Windows-only PowerShell tool, so not adopted; a Linux
  xemu tool is a TODO. README reworked: current progress, Linux-first (Steam Deck), Windows next, macOS later.
- 2026-10-18 – **Rumble.** The emulated USB pad dropped every OUT packet. Runtime patch: `usb_gamepad_output()` parses
  the XID output report (`00 06`, left/low motor u16, right/high motor u16) and calls `xbox_InputSetState()`; ohci.c
  calls it for endpoint-2 OUT data and for the data stage of HID SET_REPORT (`21 09`) on ep0. SSX 3 sends rumble
  **only over the control pipe** (`RECOMP_USB_STATS`: out0 grows, outN 0) – an endpoint-2-only fix would have done
  nothing. Seen in a race: decaying low-motor pulses 65535 → ~20000 → 0, 171 changes in ~30 s of riding.
  - `ssx3_pad.c`: SDL rumble with the maximum duration (0xFFFF ms), renewed after 30 s while non-zero, nothing sent
    while the speeds stay the same (the game repeats them; each send is a HID write). `SSX3_PAD_TRACE=1` logs changes.
  - `scripts/test_pad.sh` checks rumble on the virtual pad. Quirk: under sdl2-compat (2.32.50+) the virtual pad's
    rumble callback must return non-zero for success (SDL3 bool), real SDL2 wants 0 – the test picks by version.
  - Not verified: the feel on a real pad (8BitDo was switched off during the test).
- 2026-10-19 – **Frame interpolation above 60 fps** (branch `feature/unlocked-framerate`, design and numbers in
  `docs/frame-interpolation.md`). The simulation is a fixed 60 Hz step: timer object vtable 0x002F5300,
  `sub_001AE480` = Init(hz) (period 1000/hz), app runner `sub_001ADAC0` sets app+0x14 = 60, app+0x18 = 1/60;
  `sub_001AD880` increments app+0x20 per state update; 1/60 (0x3C888889) hard-coded in ~50 code sites → no faster tick.
  Instead the Vulkan back end records each frame, and a presenter thread re-renders it with positions blended from the
  previous frame (pairing by key + UV signature, borrowed motion for unpaired 3D draws against cracks). Timed by the
  game's step count via a wrapper on `sub_001F3020` (indirect-only, `src/recomp_manual.c`). Results: steady 120 fps
  with `SSX3_FPS_LIMIT=120`, 266–271 fps at the default cap (270 Hz display), 0 holds in races, game fps unchanged vs.
  `SSX3_INTERP=0`. Pitfalls found on the way: first-in-order pairing mismatched identical terrain chunks (cracks);
  `vkGetFenceStatus` polling needed so the NV2A thread finds free packets; fixed 1/60 per step drifts (timer has ms
  resolution) → measured step length; schedule latency must be pulled back towards the cushion or it sticks at the
  maximum. Debug: `SSX3_VKI_DUMP`, `[VKI]` log lines every 5 s.

- 2026-10-19 – **Internal resolution 1x–5x** (`SSX3_RES_SCALE=1..5`, default 1; **F10** cycles it in game).
  `vk_backend.c`: every guest surface is a GPU image of guest size × scale (`surface_extent`); depth, the
  presenter's target and the render-target snapshots follow. Vertices stay in guest pixels (the shaders divide by
  the guest size), so only image sizes, viewports, scissors and blits change; clears cover the whole image and the
  executor delivers triangles only (no point sizes/line widths to scale). A new scale is applied at a flip, and the
  old images are freed at the start of the next frame (`purge_scaled`). Widescreen hook: `R.wide` (horizontal factor,
  widens image + display aspect; unset – needs the camera projection too, see koen's `PerspectiveFovLH` 0x0024EA13).
  Same approach as koen's `RECOMP_RES` (findings only).
  - **Frames in flight (2).** Scaling exposed a CPU/GPU serialisation: `ensure_recording` waited for the previous
    frame's fence – 6–8 ms per frame at 3x vs ~2 ms at 1x (probe), game 46 → 34 fps in a heavy scene. Now two command
    buffers/fences/acquire semaphores/vertex+staging rings alternate. After: `SSX3_INTERP=0` game fps 1x 56–60,
    3x 59–60, 5x 53–60 (RTX 2080 Ti, same route). Default (interpolation, 2x): 270 presented / game 60.
  - Open: at 5x with interpolation at the display's refresh (270 Hz) the presenter saturates the GPU and the game
    drops to ~25 fps; `SSX3_FPS_LIMIT=120` keeps it at 60 → the presenter needs a GPU budget. Downscaling 4x/5x
    into a small window is a single linear blit (aliasing, not true SSAA). Not checked: Vulkan validation layer
    (not installed here). The strips at the left/right edge of the title screen look the same at 1x (pre-existing).
  - Tests now use a virtual uinput Xbox 360 pad (python-evdev) + `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`
    instead of xdotool, so runs don't take the desktop's focus.
- 2026-10-19 – **Start-up menu in `scripts/run.sh`** (`scripts/_launch_menu.sh`): asks for the internal resolution
  (`SSX3_RES_SCALE` 1–5) and frame rate (display refresh / 60 = `SSX3_INTERP=0` / 120–240 = `SSX3_FPS_LIMIT` /
  uncapped = `SSX3_FPS_LIMIT=0`) when stdin+stdout are a terminal and the renderer is Vulkan. whiptail or dialog when
  installed, otherwise a built-in arrow-key menu (SteamOS may have neither). Last choice in
  `${XDG_CONFIG_HOME:-~/.config}/ssx3-recomp/run.conf`, reused without a terminal; env vars win and are not asked;
  `SSX3_MENU=0/1` never/always. Tested all three menu back ends through a pty (arrow keys, number keys, Esc =
  quit, saved defaults, no-TTY, env override); not yet tried on a Deck.
- 2026-10-19 – **V-sync in the start-up menu + `scripts/run.ps1`.** `_launch_menu.sh` asks a third question,
  v-sync off (mailbox, immediate as fallback) / on (FIFO) → `SSX3_VSYNC`, saved as `VSYNC=` in `run.conf`; a set
  `SSX3_VSYNC` is not asked, an old `run.conf` without the key means off, a bad saved value is ignored. Tested
  through a pty (built-in menu, whiptail, dialog, reuse without a TTY, env override, quit). `scripts/run.ps1` is the
  Windows port of `run.sh` + the menu (PowerShell 5.1/7, built-in arrow-key menu, `%APPDATA%\ssx3-recomp\run.conf`).
  It defaults to `SSX3_RENDERER=cpu` + `RECOMP_FB_WINDOW=1`, because CMake builds the Vulkan back end only for
  non-WIN32 (no Win32 surface yet), so the menu only shows with `SSX3_RENDERER=vulkan`. **Not run yet**: no
  PowerShell on this machine. TODO entry added for a true high frame rate in the game logic (instead of interpolation,
  which costs ~24 ms of display latency).
- 2026-10-19 – **First release v0.1.0 + license.** Our own code is now **GPL-3.0-or-later** (`LICENSE`; koen's
  GPL-2.0-or-later code may be adopted with its copyright header, see CLAUDE.md). `scripts/package_release.sh <ver>`
  builds `dist/SSX3-Recomp-<ver>-{linux-x86_64.tar.gz,windows-x86_64.zip}` (binary, launcher scripts, player
  README, `game_files/README.txt`, licenses – never game data). Linux is built in an Ubuntu 24.04 container
  (`docker/Dockerfile`, `scripts/docker.sh`): the host build (Arch, GCC 16) needs GLIBC_2.43 (`sqrtf`), too new for
  SteamOS; the container build needs at most GLIBC_2.38. `scripts/docker.sh <cmd>` runs any repo script in that image
  (pipeline with its own venv via `SSX3_VENV`, `build.sh` → `build-docker/`, `build_win.sh` → `build-docker-win/`).
  Windows: MinGW's `libxinput.a` imported `XINPUT1_3.dll` (DirectX 2010 redist only, missing on stock Windows) →
  CMakeLists links `xinput1_4` for MinGW, also in `xbox_input`'s interface. `run.sh` / `run.ps1` work from the repo
  and from an unpacked package (binary next to the script) and stop with a message if `game_files/default.xbe`
  is missing; `run.bat` wraps `run.ps1` for double-clicking. README rewritten for players and builders.
- 2026-10-19 – **Release v0.1.0 test (downloaded from GitHub).** Linux tarball: `run.sh` start-up menu (whiptail,
  through a pty; choice saved to the given `XDG_CONFIG_HOME`), virtual uinput pad on port 1, intro → title (Start) →
  character select → Snow Jam race, Vulkan 270 Hz presented / game 59–60 fps (pre-race screen ~33–42). Windows zip
  under GE-Proton (exe started with `run.ps1`'s defaults – `run.ps1`/`run.bat` themselves not run: no PowerShell
  here): pad via `XINPUT1_4` works, intro → title → main menu → peak/event select → My Rules at 10–22 fps (CPU
  renderer), then **crash entering the race**: write to Xbox VA 0xF0610000 – the tiled aperture at 0xF0000000 failed
  to map (`error 487`, as did RAM mirrors 26/27). Documented as a known issue (README, release notes, TODO).
  Follow-up the same day with PowerShell 7.6 installed (`pwsh` on Linux; `.exe` runs through binfmt → Wine): the
  package's `run.ps1` works – missing `game_files\default.xbe` stops with the message; with `SSX3_RENDERER=vulkan`
  the start-up menu (arrow keys, Enter) saves `RES_SCALE=2 / FPS=120 / VSYNC=0` to `%APPDATA%\ssx3-recomp\run.conf`
  and starts the exe; the default flow (CPU renderer, no menu) under plain Wine reaches intro → attract → title →
  menus → "Loading…" and crashes like under Proton (exit 139). PowerShell needs the terminal to answer `ESC[6n`
  (cursor position) – a pty driver has to reply, or the menu waits forever.
- 2026-10-19 – **Widescreen: 16:9, 16:10, 21:9, any W:H, auto** (`SSX3_ASPECT`, default 4:3 = off; branch
  `widescreen`). Mechanism from koenhendriks/SSX3-recomp (CLAUDE.md "WIDESCREEN", findings only, own code):
  - Camera: D3DX `PerspectiveFovLH` 0x0024EA13 (stdcall out, fovy, aspect, zn, zf) is called by the renderer's
    camera setup sub_001EAB40 (only via vtable – no direct callers in our gen code; return address 0x001EACA5) with
    aspect 640/480. `recomp_manual.c` `sub_0024EA13_enter` multiplies the aspect at `[esp+12]` by
    `g_ssx3_wide` = shape / (4/3) – the game renders and culls a wider view squeezed into 640x480. Verified: the
    constructor sub_000CB800 installs vtable 0x00299D7C (= the in-game app state, app object at 0x003A930C, current
    state at +4) – `ssx3_in_game()`; widescreen only applies there.
  - Entry hooks need the recompiler's `--exclude-manual` (`run_pipeline.sh` passes `src/recomp_manual.c` now; it
    emits `sub_X_enter()` at the top of generated `sub_X`, and keeps `sub_X_gen` wrappers possible). **Regenerate
    (`scripts/run_pipeline.sh recomp`) before building this branch.**
  - `vk_backend.c`: at a flip `wide_update` sets `R.wide` = requested factor while in game, else 1. Screen-sized
    surfaces (guest size of the shown surface, 640x480) get images `R.wide` times wider and a display aspect to
    match; offscreen targets (shadow maps 128/124, a 640x476 pass) stay as they are. A change frees the old
    screen images and all depth images at the next frame start (`purge_scaled`, `R.purge = 2`). Push constant
    `squeeze` = 1/wide: `nv2a.vert` narrows 2D vertices (rhw 1, the HUD) to the 4:3 middle unless they are on the
    left/right edge (full-screen passes). Menus, loading screens and movies stay 4:3 and are pillarboxed in the
    window by the existing `fit_rect`. The window opens 720 high at the chosen shape (1280x720 at 16:9).
  - Start-up menu (`_launch_menu.sh`, `run.ps1`): new "Aspect ratio" question (4:3 / 16:9 / 16:10 / 21:9 / auto,
    default 16:9) → `SSX3_ASPECT`, saved as `ASPECT=`; the saved-value parser now accepts `:`.
  - Tested (Linux, virtual pad, default flow with the intro skipped by Start): 16:9 at 1x → 853x480 race frames,
    HUD in proportion in the middle, menus 640x480; 21:9 at 2x → 2240x960, full-screen tint passes reach the edges;
    a saved `ASPECT=16:10` reaches the back end (x1.200). Not tested: Windows build (the Vulkan back end is not
    built there yet; `g_ssx3_wide` stays 1 with the CPU renderer), split screen, replays/cut-scenes in game, HUD
    elements anchored to the screen edge (a quad with one vertex on the edge would be stretched).
- 2026-10-19 – **Display modes** (`SSX3_WINDOW=windowed|borderless|fullscreen`, default windowed; branch
  `display-mode` on top of `widescreen`). `vk_backend.c` `window_create` (both window paths): borderless =
  `SDL_WINDOW_FULLSCREEN_DESKTOP`, fullscreen = `SDL_WINDOW_FULLSCREEN` at the desktop's own mode (no resolution
  switch; the frame is scaled as before), cursor hidden in full screen. **Alt+Enter / F11** toggle windowed <->
  the chosen full-screen mode (borderless when started windowed); the swapchain follows the resize. Start-up menus
  (`_launch_menu.sh`, `run.ps1`): first question "Display mode", saved as `WINDOW=`.
  - Fixed on the way: `ssx3_vk_host_main` opens the window before the guest calls `ssx3_vk_register`, so the
    widescreen window size never applied (always 1280x960) – `window_create` now runs `wide_init` itself.
  - Tested inside `gamescope --backend headless` (keys sent with xdotool to gamescope's own X display, not the
    desktop): windowed 1280x720 (16:9) / 1680x720 (21:9), borderless and fullscreen 1920x1080, F11 and Alt+Enter
    switch both ways (gamescope keeps every window at its full size, so "windowed" stays 1920x1080 there). Not
    tested: on a real desktop session, Windows, multi-monitor (always display 0). Alt+Enter also reaches the
    keyboard-as-pad mapping as Enter.
- 2026-10-19 – **Monitor choice** (`SSX3_MONITOR=primary|<number>|<part of name>`, default primary; branch
  `display-mode`). Cause of "opens on the 16:10 monitor": SDL 2 runs on Wayland here and has no primary monitor
  there – display 0 is the first output KWin announces (HP ZR24w 1920x1200), while the KDE primary is DP-2 (ASUS
  VG279QM, 1920x1080 at 0,0). `vk_backend.c` `display_primary` asks Xwayland's RandR monitor list (KDE/GNOME
  mirror the primary there; libX11/libXrandr via dlopen, no build dependency) and takes the SDL display at the
  primary's position; no X → display 0. The window is created with `SDL_WINDOWPOS_CENTERED_DISPLAY(d)`; borderless/
  fullscreen use that display's desktop mode, `SSX3_ASPECT=auto` its shape. `ssx3_recomp --list-displays` prints
  number / name / mode / Hz / primary (tab-separated, before any boot work) for the menus; `run.sh` and `run.ps1`
  ask "Monitor" first when there are two or more and save the name (`MONITOR=`; the saved-value parsers accept
  names with spaces, dots, commas, brackets). The swapchain log line now names the window's display.
  - Tested: on this desktop `--list-displays` marks the ASUS (SDL display 1) primary; nested headless KWin with 3
    virtual outputs (own dbus + config dir): primary/1/2 × borderless/fullscreen each land on the chosen output;
    built-in menu through a pty (choice saved and reused), `Get-MonitorItems` under pwsh. Not testable nested: a
    primary other than output 0 (KWin owns Xwayland's primary). Windowed placement on Wayland stays the
    compositor's decision.
- 2026-10-19 – **Release process as a skill.** `.claude/skills/release/SKILL.md` (`/release [version] [--yes]`):
  preconditions, version, changelog since the last tag → `docs/release-notes/<version>.md`, `package_release.sh`,
  `test_release.sh`, notes commit, review (skipped with `--yes`), tag + `gh release create --latest`.
  `scripts/test_release.sh <version>` (helpers in `tests/release/`: `drive.py` = pty + virtual uinput pad, answers
  `ESC[6n`; `contact_sheet.py`) automates this session's checks: missing-game-files message, `run.sh` menu
  (`SSX3_MENU=1`), Linux race with Vulkan dumps; Windows: `run.ps1` menu + default flow under Wine (own prefix in the
  results folder). Run on v0.1.0: Linux reaches a race (game 35–60 fps), Windows menus then exit 139 as before.
- 2026-10-19 – **Renderer choice in the start-up menus + renderer in the window title.** `ssx3_recomp --renderers`
  lists the back ends the build has, preferred first (`vulkan` + `cpu` on Linux, `cpu` only on Windows; handled
  before any boot work, in WinMain through the command line). `_launch_menu.sh` / `run.ps1` ask "Renderer" first
  when there are two, saved as `RENDERER=` in `run.conf` (a saved value this build lacks falls back to the first
  listed); `run.sh` / `run.ps1` now apply the `SSX3_RENDERER` default after the menu. The software renderer has none
  of the other options (monitor, display mode, resolution, aspect ratio, frame rate, v-sync are all `vk_backend.c`;
  the widescreen projection patch reads `g_ssx3_wide`, which only the Vulkan back end sets), so choosing it ends the
  menu; the other saved values are kept for switching back. `SSX3_RENDERER` in the environment wins, as before.
  The framebuffer window was titled after the XBE certificate ("SSX 3"); `boot_memory` now renames it
  "SSX 3 (Recomp, Software)", matching "SSX 3 (Recomp, Vulkan)". `test_release.sh` sends one more Enter on Linux.
  - Tested: built-in menu through a pty (software chosen → no further questions, saved; reused without a tty; env
    `SSX3_RENDERER=cpu` → no menu; back to Vulkan → all 7 questions), both renderers boot from `run.sh`, titles read
    with xprop (SDL_VIDEODRIVER=x11); MinGW build: `--renderers` = `cpu`, `run.ps1` under Wine with a saved
    `RENDERER=vulkan` → software. Not tested: whiptail/dialog front ends (same `_menu` call), native Windows.
