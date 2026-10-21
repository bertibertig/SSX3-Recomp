# Local patches for the xboxrecomp submodule

The submodule (`xboxrecomp/`, v0.14.0) is not committed in modified form. Our own changes live here as a patch.
`scripts/build.sh`, `build_win.sh` and `run_pipeline.sh` apply these automatically (`apply_runtime_patches` in
`scripts/_common.sh`): skipped when already applied, applied on a fresh clone / after a submodule update, a warning
when the submodule has other local edits (runtime work in progress). By hand, apply them in order:

```bash
git -C xboxrecomp apply ../patches/xboxrecomp/0001-linux-x86_64-mmio-traps-casefold-mingw-oldnames.patch
git -C xboxrecomp apply ../patches/xboxrecomp/0002-linux-installer-python-venv.patch
```

After changing the runtime, re-export the patch so the change is kept:

```bash
git -C xboxrecomp diff -- . ':!tools/linux' > patches/xboxrecomp/0001-linux-x86_64-mmio-traps-casefold-mingw-oldnames.patch
```

All of them are candidates for an upstream PR to sp00nznet/xboxrecomp.

## 0001 – Linux x86-64: trapped MMIO, case-insensitivity, MinGW old names
- `src/platform/mmio_decode.h`: x86-64 decoder no longer restricted to `_WIN32`. Context abstracted
  (`mmio_ctx_t`, `MMIO_CTX_PC`, `MMIO_CTX_EFLAGS`, `mmio_ctx_reg`): Win32 `CONTEXT` or Linux `ucontext_t.gregs[REG_*]`.
  Decoder logic unchanged.
- `src/apu/apu_mmio_hook.c`: APU MMIO hook on Linux x86-64 as well, new entry point `apu_hook_handle_mmio_posix`.
- `src/kernel/xbox_memory_layout.c`: `xbox_PosixMmioFault` and the AC'97 bus-master write trap on Linux x86-64
  (previously arm64 only) – without this, XDK DirectSound crashes with `RECOMP_AC97_READY=1` on the first APU register.
- `src/usb/ohci.c`: OHCI trap on Linux x86-64 (`OHCI_TRAP_POSIX_X64`).
- `src/kernel/kernel_path.c`: POSIX `xbox_translate_path` resolves path components case-insensitively when the
  exact path is missing (FATX/XDVDFS and Win32 are case-insensitive; SSX 3 opens `intro_dj.mad`, on the disc it is called
  `Intro_DJ.mad`).
- `tools/recomp/lifter.py`: MSVC "old names" from MinGW `math.h` (`chgsign`, `fpclass`, `finite`, `j0/j1/jn/y0/y1/yn`)
  in `_FUNC_RESERVED_IDENT` – otherwise a MinGW build breaks on Ghidra names like `fpclass`.

- `src/kernel/nv2a_pb_exec.c`: **disabled vertex attributes** deliver, as vertex-program input, the value last set via
  `SET_VERTEX_DATA*` (NV2A/xemu behaviour) instead of (0,0,0,1). SSX 3 passes the matrix of its
  2D/FMV program in v8..v15 via `SET_VERTEX_DATA4F_M` – without this every vertex was transformed to the same point
  and the intro video was discarded as degenerate triangles. Plus `RECOMP_VSH_TRACE_EVERY=<n>` (stride of the VSH trace,
  default 20000 as before).

- `src/kernel/nv2a_pb_exec.c`: **rasteriser thread pool on POSIX too** (pthreads; previously `_WIN32` only, Linux rendered
  single-threaded) – SSX 3 title screen 4.4 → 15.4 fps; pool size as on Windows (`RECOMP_RASTER_THREADS`, default CPUs−4).
  Plus the executor's fps display uses wall-clock time (`GetTickCount64`) instead of `clock()` (= process CPU time on POSIX,
  which showed several times too few fps with multiple busy threads).

- `src/kernel/nv2a_pb_exec.c` (`backend_flush`): with register combiners in use, a back end only receives the stage-0
  texture when that stage's `SHADER_STAGE_PROGRAM` is not NONE (as the CPU path does in `rc_stage_fetch`) – otherwise
  the Vulkan back end multiplied SSX 3's title-screen sky by the left-over snowflake atlas and drew it black.

- `src/kernel/nv2a_backend.h`, `nv2a_pb_exec.c` (`backend_flush`, `backend_from_xf`): **back-end interface
  extension** `Nv2aBatchExt` for transformed batches: register-combiner state, all four texture stages, and per vertex
  the four texture-coordinate sets, specular colour and fog factor (what the CPU path feeds `nv2a_rc_eval`).
  `Nv2aBatch.ext` is NULL for screen-space batches; a back end that ignores it draws as before.
- `src/kernel/nv2a_pb_exec.c` (`raster_batch_program`): **post-transform vertex cache** per batch (direct mapped by
  index, off while CXT_WRITE is on): indexed meshes ran the vertex program once per index instead of once per vertex.
- `src/kernel/nv2a_vsh_interp.c`: **programs are decoded once** (fields extracted into `VshIns` when the program or
  start slot changes) instead of per source per vertex, and sources an op does not read are not fetched.
  New `nv2a_vsh_cxt_write_enabled()`. Together with the cache: SSX 3 race 23 → 55–60 fps on the Vulkan back end.
- `src/kernel/xbox_memory_layout.c` (`xbox_Nv2aRaiseVblank`, `xbox_Nv2aAckBusyBits`), `kernel_bridge.c`
  (`kernel_vblank_tick`): **the vblank stays pending until the title acknowledges it.** The ack thread held every
  interrupt status register at zero and often wiped PMC_INTR_0 bit 24 / PCRTC_INTR_0 bit 0 between the timer thread
  raising the vblank and the D3D ISR's DPC reading it; the DPC then skipped its vblank work and never set the event
  `D3DDevice_BlockUntilVerticalBlank` waits on. SSX 3 waits there once per `AvSetDisplayMode` step, so boot took
  0.7 s to over 70 s at random. Now the raise also sets a marker bit (31) in PCRTC_INTR_0 that D3D's ack (a write
  of exactly 1) removes; only then are the bits cleared (fallback after 100 ms). Boot: 0.5–0.6 s every run.
- `src/kernel/nv2a_pb_exec.c` (`backend_flush`): an `Nv2aTexture` carries a pitch only for linear formats. A
  swizzled/DXT stage keeps the pitch its stage last had (SSX 3's intro video: 640x480 linear), and the Vulkan back end
  took pitch != 0 for "linear" -- after the intro, the title screen sampled the corner of its textures (only the sky
  was left).
- `src/usb/ohci.c`, `usb_gamepad.c/.h` (`usb_gamepad_output`): **rumble.** The emulated pad's OUT packets were
  counted and dropped. Output reports (`00 06 <left u16> <right u16>`) now go to `xbox_InputSetState()`, both on
  interrupt endpoint 2 and as the data stage of a HID SET_REPORT (`21 09`) on the control pipe -- SSX 3 uses the
  latter only (`out0` grows, `outN` stays 0 in `RECOMP_USB_STATS`).
- `src/kernel/nv2a_backend.h`, `nv2a_pb_exec.c` (`current_surface`): `Nv2aSurface` carries **`clip_x`/`clip_y`**.
  Width/height were the clip size only, so a back end could not tell a 124x124 clip at 2,2 (SSX 3's shadow-map
  draws) from a separate 124x124 surface; the Vulkan back end now sizes surfaces by clip offset + size.
- `src/kernel/nv2a_pb_exec.c` (`attr_inline`, `fetch_attr`, `transform_vertex`): a vertex-program input with a
  size but **stride 0** is an array whose one element every vertex shares, not a disabled attribute. SSX 3's rider
  program adds v4 (size 3, stride 0, pointing at 0,0,0) before skinning; read as the immediate SET_VERTEX_DATA
  value, it took a world offset other draws had set, and from the Snow Jam lift station on the rider's body vanished.

Verified: Linux build + Windows cross build (MinGW) compile; own decoder test (8 cases, ucontext) green;
`tools.recomp.test_reserved_idents` green; SSX 3 on Linux reaches the same state as under Proton
(APU/AC'97/DSP doorbell/APU interrupts, intro videos are opened).
- `src/kernel/kernel_file.c` (POSIX `xbox_NtCreateFile`, `xbox_NtDeleteFile`, `xbox_NtQueryFullAttributesFile`,
  new `posix_obj_path`): **names relative to a RootDirectory handle** are joined to that handle's host path, as the
  Win32 backend already did (`translate_obj_path`). XAPI opens a save game's files relative to the save directory;
  through the path table alone the bare `Data.ssx` failed with STATUS_OBJECT_PATH_NOT_FOUND and SSX 3 showed
  "Unable to save" when overwriting an existing save (Linux only; the Windows build was not affected).
  Proposed upstream with a test (`tests/kernel_file_relative`): https://github.com/sp00nznet/xboxrecomp/pull/237 –
  drop this part once it is in a release we update to.
- `src/apu/apu_vp.c`: **idle-voice traps round-robin** (from Amped2-Recomp, 2026-10-21) – one SE2FE_IDLE_VOICE
  trap per frame as before, but for the first idle voice *after* the one reported last instead of always the first
  in list order. DirectSound (XDK 5558; service routine SSX 3 0x0025C569, Amped 2 0x002936B5) retires a buffer with
  several hardware voices through the trap of one of them and ignores the others. In Amped 2 such a pair stayed
  idle on the results screen: the ignored voice trapped on every frame (~1500/s), no other idle voice was ever
  reported, and DirectSound's waits for stopped voices ran into their 0.5 s timeout (5-40 fps). SSX 3 has the same
 routine but never left an idle voice in a list in our runs (menus + a 4.5-min race: 0 idle-voice traps), so this
 is a latent fix here. `RECOMP_APU_IDLE_LOG=1` prints the idle-voice traps per voice every 5 s.

Ported from Amped2-Recomp (commit 277f07e, same text there):
- `kernel_hal.c`: **dispatch lock** – DISPATCH_LEVEL now excludes DPCs, as on the Xbox's one CPU. A thread holds a
  global lock from the moment its IRQL reaches DISPATCH_LEVEL until it drops below; a DPC (entered through
  `xbox_IrqlEnterInterrupt(2)`) takes it too, ISRs (device IRQL) do not. DirectSound's lock is a raise to
  DISPATCH_LEVEL: its stream DPC used to run between a game thread's SET_CURRENT_VOICE and the following volume / SSL
  writes, so they landed on the wrong voice (Amped 2: music mono, quiet or silent, no next track, static, a crash).
  A DPC waits at most `RECOMP_DISPATCH_LOCK_MS` (100) for a raised thread, then runs anyway with a warning and the
  `[IRQLHOLD]` holder list; `RECOMP_DISPATCH_LOCK=0` turns it off.
- `apu_dsp.c`: **stereo downmix of the speaker bins** – FL/FR + C and BL/BR at -3 dB to both sides, crosstalk bins
  6..9 as their speakers, LFE left out, the I3DL2 send (10) at -6 dB to both sides; FX sends 11..31 even/odd as
  before. The even/odd fold put LFE on the right only and the rears at full level (Amped 2's music: left 1.7x,
  right 2.7x, clipping). `RECOMP_APU_MIXDOWN=legacy` restores the even/odd fold.
- Audio diagnostics (`apu_vp.c`, `apu_dsp.c`, `apu_core.c`): under `RECOMP_APU_TRACE` a `[APU-VROUTE]` line per voice
  (list, envelope, pauses, all eight bin/volume pairs; multipass voices included) and `[APU-BINS]` (RMS per mixbin,
  samples the mixdown clamped) every second; `RECOMP_APU_TRACE_NO_MMIO=1` keeps those without the per-access register
  log; `RECOMP_APU_SSL_LOG=1` logs every stream list set/done/starve with a timestamp; `RECOMP_APU_VOICE_LOG=<voice>`
  logs every front-end method naming that voice (with the writer's guest stack for volume writes).
- `apu_vp.c` (with the above, as in Amped2-Recomp): `RECOMP_APU_SSL_STATS=1` prints stream-list totals every 5 s
  (lists set, played+notified, starved fetches). SSX 3 sets no stream lists (0/0/0 all race); its music does not
  go through SSLs.
  SSX 3 check: no "DPC waited" warning in any run, fps unchanged; see docs/notes.md 2026-10-21.

## 0002 – Linux installer Python prerequisites
- `tools/linux/install_deps.sh`: include Python 3 and `venv` support in the Debian/Ubuntu package list.
- Mark the installer and its distro-detection test executable.
