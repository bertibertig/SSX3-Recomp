# Local patches for the xboxrecomp submodule

The submodule (`xboxrecomp/`, v0.14.0) is not committed in modified form. Our own changes live here as a patch.
`scripts/build.sh`, `build_win.sh` and `run_pipeline.sh` apply it automatically (`apply_runtime_patches` in
`scripts/_common.sh`): skipped when already applied, applied on a fresh clone / after a submodule update, a warning
when the submodule has other local edits (runtime work in progress). By hand:

```bash
git -C xboxrecomp apply ../patches/xboxrecomp/0001-linux-x86_64-mmio-traps-casefold-mingw-oldnames.patch
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

Verified: Linux build + Windows cross build (MinGW) compile; own decoder test (8 cases, ucontext) green;
`tools.recomp.test_reserved_idents` green; SSX 3 on Linux reaches the same state as under Proton
(APU/AC'97/DSP doorbell/APU interrupts, intro videos are opened).
