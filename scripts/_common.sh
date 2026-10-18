#!/usr/bin/env bash
# Gemeinsame Pfade für alle Skripte (wird mit `source` geladen).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="$ROOT/xboxrecomp"
GAME_FILES="$ROOT/game_files"
GEN_DIR="$ROOT/src/recomp/gen"
VENV="$ROOT/.venv"
XBE="$GAME_FILES/default.xbe"
export PYTHONUNBUFFERED=1   # Logs live schreiben (sonst puffert Python bei Umleitung in Dateien)

use_venv() {
  if [ -f "$VENV/bin/activate" ]; then
    # shellcheck disable=SC1091
    . "$VENV/bin/activate"
  else
    echo "Kein .venv gefunden – bitte zuerst: scripts/setup.sh" >&2
    exit 1
  fi
}

need_xbe() {
  if [ ! -f "$XBE" ]; then
    echo "game_files/default.xbe fehlt. Erst: scripts/extract_iso.sh <SSX3-Xbox.iso>" >&2
    exit 1
  fi
}
