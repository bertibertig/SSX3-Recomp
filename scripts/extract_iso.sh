#!/usr/bin/env bash
# Entpackt dein eigenes SSX3-Xbox-Disc-Image nach game_files/.
#   scripts/extract_iso.sh "/pfad/zu/SSX 3 (Xbox).iso"
source "$(dirname "$0")/_common.sh"
ISO="${1:?Usage: scripts/extract_iso.sh <image.iso>}"
[ -f "$ISO" ] || { echo "Datei nicht gefunden: $ISO" >&2; exit 1; }
mkdir -p "$GAME_FILES"
if command -v extract-xiso >/dev/null; then
  extract-xiso -x "$ISO" -d "$GAME_FILES"
elif command -v xdvdfs >/dev/null; then
  xdvdfs unpack "$ISO" "$GAME_FILES"
else
  echo "Weder extract-xiso noch xdvdfs gefunden." >&2
  echo "  extract-xiso: https://github.com/XboxDev/extract-xiso" >&2
  echo "  xdvdfs:       https://github.com/antangelo/xdvdfs  (cargo install xdvdfs-cli)" >&2
  exit 1
fi
ls -la "$GAME_FILES" | head -20
[ -f "$XBE" ] && echo "OK: default.xbe gefunden" || echo "WARNUNG: default.xbe nicht im Wurzelverzeichnis"
echo "SHA-1 der XBE (für docs/notes.md notieren):"; sha1sum "$XBE" 2>/dev/null || true
