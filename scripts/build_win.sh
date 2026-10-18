#!/usr/bin/env bash
# Windows build (D3D11/XInput/XAudio2) via the MinGW-w64 cross compiler, e.g. for testing under Proton.
#   scripts/build_win.sh            # → build-win/ssx3_recomp.exe
# Needs: sudo pacman -S --needed mingw-w64-gcc   (xboxrecomp ships the toolchain file)
source "$(dirname "$0")/_common.sh"
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc missing: sudo pacman -S --needed mingw-w64-gcc" >&2; exit 1; }
[ -f "$GEN_DIR/recomp_funcs.h" ] || { echo "No generated code. First run: scripts/run_pipeline.sh" >&2; exit 1; }
cmake -S "$ROOT" -B "$ROOT/build-win" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLKIT/cmake/mingw-w64-x86_64.cmake"
cmake --build "$ROOT/build-win" --parallel "${JOBS:-$(nproc)}"
