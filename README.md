# SSX3-Recomp

Experimental **static recompilation of SSX 3 (original Xbox)** into a native x86-64 PC program,
based on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (as a Git submodule under `xboxrecomp/`).

> This repo contains **no game code and no game data**. You need your own, legally obtained
> SSX 3 Xbox disc image. The C code is generated locally from your `default.xbe` and never committed.

Status: setup prepared, pipeline not run yet. See `docs/notes.md`.

## Quick start

```bash
git clone --recurse-submodules <this-repo>       # or locally: git submodule update --init --recursive
cd SSX3-Recomp

scripts/setup.sh                                  # Python venv + capstone, tool check
bash xboxrecomp/tools/linux/install_deps.sh       # Linux only: SDL2, libepoxy, OpenSSL, ffmpeg (sudo)

scripts/extract_iso.sh "/path/to/SSX 3 (Xbox).iso"   # -> game_files/default.xbe
scripts/run_pipeline.sh parse                     # entry point etc. -> src/main.c + docs/notes.md
scripts/run_pipeline.sh                           # disasm, func_id, abi, recomp -> src/recomp/gen/
scripts/doctor.sh                                 # bring-up report
scripts/build.sh                                  # CMake build
```

The first start is expected to crash – that is where the iterative work begins
(see `xboxrecomp/docs/GETTING_STARTED.md`, steps 7–10).

## Working with Claude Code

Start `claude` in the repo root: `CLAUDE.md` explains the goal, layout, workflow and rules.
Good first tasks:

1. "Run setup and the pipeline step `parse`, and fill the XBE values into `src/main.c` and `docs/notes.md`."
2. "Compare `reference/OpenTricky/xboxrecomp` with `xboxrecomp/` and list which port changes are still missing upstream."
3. "Run the complete pipeline, then `scripts/doctor.sh`, and summarise the biggest gaps."

## VS Code

Open the folder (`code .`) and install the recommended extensions. Under *Terminal → Run Task* the
pipeline steps are available as tasks. The generated code in `src/recomp/gen/` is excluded from search and the file watcher (very large).

## References

- xboxrecomp: https://github.com/sp00nznet/xboxrecomp
- SSX Tricky PC (Xbox recomp of the predecessor): https://github.com/MatiasRiveraC/SSX-Tricky-PC
- OpenTricky: https://github.com/GiZcesi/OpenTricky
- SSX3 PS2 decomp: https://github.com/ssxdecomp/ssx3 · GameCube: https://github.com/ssxdecomp/ssx3_gc

License of our own files: still to be decided (xboxrecomp: MIT; if code from OpenTricky is adopted: observe GPL-3.0-only).
