#!/usr/bin/env bash
# Windows-Build (D3D11/XInput/XAudio2) per MinGW-w64-Cross-Compiler, z. B. zum Testen unter Proton.
#   scripts/build_win.sh            # → build-win/ssx3_recomp.exe
# Braucht: sudo pacman -S --needed mingw-w64-gcc   (Toolchain-Datei liefert xboxrecomp mit)
source "$(dirname "$0")/_common.sh"
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc fehlt: sudo pacman -S --needed mingw-w64-gcc" >&2; exit 1; }
[ -f "$GEN_DIR/recomp_funcs.h" ] || { echo "Kein generierter Code. Erst: scripts/run_pipeline.sh" >&2; exit 1; }
cmake -S "$ROOT" -B "$ROOT/build-win" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLKIT/cmake/mingw-w64-x86_64.cmake"
cmake --build "$ROOT/build-win" --parallel "${JOBS:-$(nproc)}"
