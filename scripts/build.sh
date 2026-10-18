#!/usr/bin/env bash
# CMake-Build des Spiels (Release). Erst nach erfolgreichem run_pipeline.sh sinnvoll.
source "$(dirname "$0")/_common.sh"
[ -f "$GEN_DIR/recomp_funcs.h" ] || { echo "Kein generierter Code. Erst: scripts/run_pipeline.sh" >&2; exit 1; }
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$ROOT/build" --parallel "${JOBS:-$(nproc)}"
