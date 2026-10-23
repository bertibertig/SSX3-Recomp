# SSX3-Recomp – Instructions for Claude Code

Goal: statically recompile **SSX 3 (original Xbox, 2003)** to C with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
and get it running as a native PC port (x86-64).
No emulator: the game code is translated to C once; the xboxrecomp runtime replaces kernel/GPU/audio/input.

## Why the Xbox version
Xbox code is already x86 (no MIPS/PowerPC translation needed). For the predecessor **SSX Tricky (Xbox)** there are
finished, working xboxrecomp ports – those are our template (see `reference/`).
PS2 (PS2Recomp: GS/VU1/DMA still stubs) and GameCube (DolRecomp: CPU only) are much further away.

## Repo layout
```
xboxrecomp/        Git submodule (sp00nznet/xboxrecomp): Python pipeline (tools/) + runtime libs (src/). Do NOT edit here;
                   changes belong in src/ (our own code) or are deliberately documented as a patch.
src/main.c         Host entry point (from templates/new-game). TODO: fill in constants from xbe_parser (see below)
src/recomp_manual.c  Hand-written overrides / stubs for functions that are not lifted correctly
src/recomp/gen/    GENERATED code (gitignored, from your own XBE) – never commit, never edit by hand
config/            Our own config, e.g. seed_functions.json (entry points the static analysis does not see)
game_files/        Extracted copy of your own disc (gitignored): default.xbe + data
reference/         Local read-only copies (gitignored): SSX-Tricky-PC, OpenTricky, ssx3-ps2-decomp, SSX3-recomp-koen
scripts/           setup.sh, extract_iso.sh, install_game.sh (+ .ps1: game data installer TUI, also in releases), run_pipeline.sh, doctor.sh, build.sh, fetch_references.sh, ghidra.sh (Ghidra names/triage, Linux)
docs/notes.md      Running work log (addresses, findings, decisions) – keep adding to it
```

## Workflow (keep this order)
1. `scripts/setup.sh` – installs Linux build dependencies, creates the venv, and installs capstone/Python packages
2. `scripts/install_game.sh <SSX3-Xbox.iso>` (or `scripts/extract_iso.sh`, needs extract-xiso/xdvdfs) → `game_files/default.xbe`
3. `scripts/run_pipeline.sh parse` – XBE header: **record entry point, base address, sections, title ID, kernel imports in docs/notes.md**
   and fill them into `src/main.c` (`YOUR_GAME_ENTRY_POINT`, `YOUR_GAME_XBE_PATH`, `YOUR_GAME_DIR`).
4. `scripts/run_pipeline.sh` (everything) – disasm → func_id → **abi_analysis (never skip!)** → recomp (`--split 250`, not 1000: RAM/OOM).
5. `scripts/doctor.sh` – shows what is still unresolved (unresolved ICALLs, stubs, ABI gaps).
6. `scripts/build.sh` → the first start **will crash**. That is normal: read stderr, evaluate the ICALL trace,
   add overrides in `src/recomp_manual.c`, seeds in `config/`, then repeat.
The guides for this: `xboxrecomp/docs/GETTING_STARTED.md`, debugging: `xboxrecomp/docs/pipeline/06-debugging.md`,
`xboxrecomp/docs/technical/indirect-calls.md`, `lessons-learned.md`.

## Platform note (important)
- **Linux first** (main target: the Steam Deck), Windows second, macOS later – all three are planned.
- Linux: our own POSIX host in `src/main.c` (sigaction MMIO faults, SDL2 window) and our Vulkan back end in `src/render/`
  (`SSX3_RENDERER=vulkan`, default). The toolkit's OpenGL path (`d3d8_gl.c`) is not used.
- Windows: xboxrecomp itself is Windows-first (WinMain, VEH, D3D11, dbghelp, XAudio2), so the Windows build is the next
  platform. Today: MinGW cross build (`scripts/build_win.sh`), tested under Proton (`scripts/run_win.sh`); the Vulkan
  back end still needs a Win32 surface (see `docs/TODO.md`), native MSVC is untested.
  `reference/SSX3-recomp-koen` is a Windows/MSVC-first port of the same game – use it for the Windows side.
- macOS: not started (Vulkan via MoltenVK; Apple Silicon would need the aarch64 MMIO decoder, see
  `reference/SSX3-recomp-koen/docs/LINUX.md`).
- The pipeline (steps 2–5) runs anywhere (Python + capstone).

## References (read first, before reinventing anything)
- `reference/SSX-Tricky-PC/` (MatiasRiveraC) and `reference/OpenTricky/` (GiZcesi): xboxrecomp ports of **SSX Tricky (Xbox)**.
  They contain an NV2A→D3D11 renderer, register combiners/vertex programs as shaders, kernel/timer/audio adjustments,
  a launcher, fix-up passes (`port/tools/regen.sh`), `port/src/seeds.json` and **lots of RE notes** in `docs/notes/`
  (archive formats, XBD/XSH/FFN, render system, boot sequence, camera, scoring …). SSX3 is a successor from the same studio/engine line –
  similarities are plausible, but **verify each one**, don't assume (SSX3 was heavily redesigned).
- These ports vendor xboxrecomp **with their own changes**. Before adopting anything, compare with the upstream state here
  (`xboxrecomp/`, v0.14.0, as of 2026-10-16): a lot of it may be upstream by now.
- `reference/ssx3-ps2-decomp/`: matching decomp of **SSX3 PS2 (NTSC-U, SLUS_207.72)** – useful for function/struct names
  and game logic via cross-reference (strings, constants), not as directly transferable code.
- `reference/SSX3-recomp-koen/` (koenhendriks/SSX3-recomp): an independent xboxrecomp port of **the same game** –
  SSX 3 Xbox PAL, title ID 0x4541003C, entry 0x00181AD2, i.e. the same XBE build, so **addresses match ours** (still
  verify). Windows/MSVC-first, playable (all peaks, split screen, saves, 60 fps, GPU vertex programs, widescreen).
  Its `CLAUDE.md` is a detailed bring-up log (root causes with guest addresses: streamer/STRM ring races, guest-CPU
  fairness, vblank/flip traps, 30 Hz flag, widescreen camera, stencil, point sprites …); `docs/ROADMAP.md`,
  `docs/LINUX.md` (aarch64), `patches/xboxrecomp-local.patch` (toolkit fixes, base v0.13.1), `src/vk/` (Vulkan renderer).
  Check it **before** debugging anything – many of our open items are solved there.
  **License: GPL-2.0-or-later** (the patch file: MIT/LGPL-2.1 like the toolkit). Findings, addresses and mechanisms may be
  used freely (cite it in `docs/notes.md`). Our own license is **GPL-3.0-or-later** (decided 2026-10-19), which is
  compatible, so its code may be adopted – keep its copyright notice in the file header and cite it in `docs/notes.md`. Its `.claude/skills/xemu` drives xemu through a Windows-only
  `tools\xemu.ps1`; a Linux equivalent would be our own tool.

## Rules
- **The repo language is English**: docs, code comments, script output, commit messages, `docs/notes.md` entries –
  even though the game itself supports English, French, German and Spanish.
- **Do not commit game data, XBE, ISO or generated code** (all in .gitignore). Only our own sources and tools.
- Never edit generated code in `src/recomp/gen/` by hand – fixes go in as an override (`recomp_manual.c`), seed or pipeline patch.
- Changes to `xboxrecomp/` (submodule) only after asking; prefer a patch/override in our own code or an upstream PR.
- Small, verifiable steps: after every change regenerate/build/run, and record the result in `docs/notes.md`.
- If numbers/addresses are unclear: prove them from `xbe_parser` output or the disassembly, never guess.
- Memory use during compilation is high: `--split 250`, reduce `JOBS=` when RAM is low.
