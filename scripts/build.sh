#!/usr/bin/env bash
# CMake build of the game (Release). Only useful after a successful run_pipeline.sh.
source "$(dirname "$0")/_common.sh"
[ -f "$GEN_DIR/recomp_funcs.h" ] || { echo "No generated code. First run: scripts/run_pipeline.sh" >&2; exit 1; }
apply_runtime_patches
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$ROOT/build" --parallel "${JOBS:-$(nproc)}"
