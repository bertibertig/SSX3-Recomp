#!/usr/bin/env bash
# Shared paths for all scripts (loaded with `source`).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="$ROOT/xboxrecomp"
GAME_FILES="$ROOT/game_files"
GEN_DIR="$ROOT/src/recomp/gen"
VENV="$ROOT/.venv"
XBE="$GAME_FILES/default.xbe"
export PYTHONUNBUFFERED=1   # write logs live (otherwise Python buffers when redirected to files)

use_venv() {
  if [ -f "$VENV/bin/activate" ]; then
    # shellcheck disable=SC1091
    . "$VENV/bin/activate"
  else
    echo "No .venv found – run first: scripts/setup.sh" >&2
    exit 1
  fi
}

need_xbe() {
  if [ ! -f "$XBE" ]; then
    echo "game_files/default.xbe missing. First run: scripts/extract_iso.sh <SSX3-Xbox.iso>" >&2
    exit 1
  fi
}
