#!/usr/bin/env bash
# Extracts your own SSX3 Xbox disc image to game_files/.
#   scripts/extract_iso.sh "/path/to/SSX 3 (Xbox).iso"
source "$(dirname "$0")/_common.sh"
ISO="${1:?Usage: scripts/extract_iso.sh <image.iso>}"
[ -f "$ISO" ] || { echo "File not found: $ISO" >&2; exit 1; }
mkdir -p "$GAME_FILES"
if command -v extract-xiso >/dev/null; then
  extract-xiso -x "$ISO" -d "$GAME_FILES"
elif command -v xdvdfs >/dev/null; then
  xdvdfs unpack "$ISO" "$GAME_FILES"
else
  echo "Neither extract-xiso nor xdvdfs found." >&2
  echo "  extract-xiso: https://github.com/XboxDev/extract-xiso" >&2
  echo "  xdvdfs:       https://github.com/antangelo/xdvdfs  (cargo install xdvdfs-cli)" >&2
  exit 1
fi
ls -la "$GAME_FILES" | head -20
[ -f "$XBE" ] && echo "OK: default.xbe found" || echo "WARNING: default.xbe not in the root directory"
echo "SHA-1 of the XBE (record it in docs/notes.md):"; sha1sum "$XBE" 2>/dev/null || true
