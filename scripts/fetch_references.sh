#!/usr/bin/env bash
# Fetches (shallow) reference repos into reference/ – read-only, not part of this repo.
source "$(dirname "$0")/_common.sh"
mkdir -p "$ROOT/reference"; cd "$ROOT/reference"
clone() { [ -d "$2" ] && echo "already present: $2" || git clone --depth 1 "$1" "$2"; }
clone https://github.com/MatiasRiveraC/SSX-Tricky-PC.git SSX-Tricky-PC   # xboxrecomp port of the predecessor
clone https://github.com/GiZcesi/OpenTricky.git OpenTricky                # continuation with a better renderer
clone https://github.com/ssxdecomp/ssx3.git ssx3-ps2-decomp               # PS2 decomp: names/structs
clone https://github.com/koenhendriks/SSX3-recomp.git SSX3-recomp-koen     # another xboxrecomp port of SSX 3 (Xbox PAL), GPL-2.0-or-later
