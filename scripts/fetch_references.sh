#!/usr/bin/env bash
# Holt (flache) Referenz-Repos nach reference/ – nur zum Lesen, nicht Teil des Repos.
source "$(dirname "$0")/_common.sh"
mkdir -p "$ROOT/reference"; cd "$ROOT/reference"
clone() { [ -d "$2" ] && echo "schon da: $2" || git clone --depth 1 "$1" "$2"; }
clone https://github.com/MatiasRiveraC/SSX-Tricky-PC.git SSX-Tricky-PC   # xboxrecomp-Port des Vorgängers
clone https://github.com/GiZcesi/OpenTricky.git OpenTricky                # Fortsetzung mit besserem Renderer
clone https://github.com/ssxdecomp/ssx3.git ssx3-ps2-decomp               # PS2-Decomp: Namen/Strukturen
