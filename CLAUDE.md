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
reference/         Local read-only copies (gitignored): SSX-Tricky-PC, OpenTricky, ssx3-ps2-decomp
scripts/           setup.sh, extract_iso.sh, run_pipeline.sh, doctor.sh, build.sh, fetch_references.sh, ghidra.sh (Ghidra names/triage, Linux)
docs/notes.md      Running work log (addresses, findings, decisions) – keep adding to it
```

## Workflow (keep this order)
1. `scripts/setup.sh` – venv + capstone; Linux build deps: `bash xboxrecomp/tools/linux/install_deps.sh` (sudo)
2. `scripts/extract_iso.sh <SSX3-Xbox.iso>` → `game_files/default.xbe`
3. `scripts/run_pipeline.sh parse` – XBE header: **record entry point, base address, sections, title ID, kernel imports in docs/notes.md**
   and fill them into `src/main.c` (`YOUR_GAME_ENTRY_POINT`, `YOUR_GAME_XBE_PATH`, `YOUR_GAME_DIR`).
4. `scripts/run_pipeline.sh` (everything) – disasm → func_id → **abi_analysis (never skip!)** → recomp (`--split 250`, not 1000: RAM/OOM).
5. `scripts/doctor.sh` – shows what is still unresolved (unresolved ICALLs, stubs, ABI gaps).
6. `scripts/build.sh` → the first start **will crash**. That is normal: read stderr, evaluate the ICALL trace,
   add overrides in `src/recomp_manual.c`, seeds in `config/`, then repeat.
The guides for this: `xboxrecomp/docs/GETTING_STARTED.md`, debugging: `xboxrecomp/docs/pipeline/06-debugging.md`,
`xboxrecomp/docs/technical/indirect-calls.md`, `lessons-learned.md`.

## Platform note (important)
- The template (`src/main.c`, `CMakeLists.txt`) is **Windows-first** (WinMain, VEH, D3D11, dbghelp).
- The pipeline (steps 2–5) runs anywhere (Python + capstone).
- Linux/macOS: the runtime builds with SDL2 + OpenGL 3.3 (`d3d8_gl.c`), which according to the docs is **less complete** than D3D11.
  For Linux, `main.c`/CMake have to be adapted – check `xboxrecomp/README.md`, `src/` (win32_compat), and how
  `reference/SSX-Tricky-PC/port/` or NFSU2-Linux solve it, before inventing anything.
- If the Linux OpenGL route gets stuck: build on Windows (MSYS2 UCRT64 or VS2022), as in `reference/*/docs/building.md`.

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

## Rules
- **The repo language is English**: docs, code comments, script output, commit messages, `docs/notes.md` entries –
  even though the game itself supports English, French, German and Spanish.
- **Do not commit game data, XBE, ISO or generated code** (all in .gitignore). Only our own sources and tools.
- Never edit generated code in `src/recomp/gen/` by hand – fixes go in as an override (`recomp_manual.c`), seed or pipeline patch.
- Changes to `xboxrecomp/` (submodule) only after asking; prefer a patch/override in our own code or an upstream PR.
- Small, verifiable steps: after every change regenerate/build/run, and record the result in `docs/notes.md`.
- If numbers/addresses are unclear: prove them from `xbe_parser` output or the disassembly, never guess.
- Memory use during compilation is high: `--split 250`, reduce `JOBS=` when RAM is low.
