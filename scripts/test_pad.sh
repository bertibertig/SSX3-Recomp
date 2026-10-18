#!/usr/bin/env bash
# Host test of the controller layer (src/input/ssx3_pad.c) with a virtual SDL
# Xbox 360 pad: hotplug, port assignment and the default button map.
# No controller or game data needed. Linux/macOS (SDL2).
source "$(dirname "$0")/_common.sh"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"
RT="$ROOT/xboxrecomp/src"
cc -std=c11 -Wall -Wextra -O1 -o "$OUT/pad_virtual" \
  "$ROOT/tests/pad_virtual.c" "$ROOT/src/input/ssx3_pad.c" "$RT/input/keyboard.c" \
  -I"$RT/input" -I"$RT" $(sdl2-config --cflags --libs)
SDL_VIDEODRIVER=dummy "$OUT/pad_virtual"
