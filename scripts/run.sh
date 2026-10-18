#!/usr/bin/env bash
# Starts the built port from the repo root (paths in src/main.c are relative to it).
#   scripts/run.sh                         # output to the console
#   scripts/run.sh 2>&1 | tee logs/run.log # for doctor.sh / seed_from_log
# Runtime switches (details: docs/notes.md, xboxrecomp/docs):
#   RECOMP_TITLE_KEVENTS=1  SSX3 builds KEVENTs itself (without: SIGSEGV in SetEvent on POSIX) – on by default here
#   RECOMP_VBLANK=1         60 Hz VBlank interrupt; D3D_BlockUntilVerticalBlank waits for it – on by default here
#   RECOMP_AC97_READY=1     AC'97 codec ready + emulated APU; without it DirectSoundCreate fails – on by default here
#   RECOMP_FB_WINDOW=1      framebuffer window (SDL2)
#   RECOMP_PB_SCAN=1 / RECOMP_PB_EXEC=1   analyse / execute the NV2A pushbuffer
source "$(dirname "$0")/_common.sh"
[ -x "$ROOT/build/ssx3_recomp" ] || { echo "No build. First run: scripts/build.sh" >&2; exit 1; }
cd "$ROOT"
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS:-1}"
export RECOMP_VBLANK="${RECOMP_VBLANK:-1}"
export RECOMP_AC97_READY="${RECOMP_AC97_READY:-1}"
export RECOMP_PB_EXEC="${RECOMP_PB_EXEC:-1}"          # execute the pushbuffer (CPU rasteriser) – otherwise no picture
export RECOMP_FB_WINDOW="${RECOMP_FB_WINDOW:-1}"      # window showing the framebuffer
export RECOMP_USB="${RECOMP_USB:-1}"                  # emulated USB controller (XPP/OHCI)
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD:-1}"        # keyboard = controller 1 (Enter=Start, Z=A, X=B, arrows=D-pad …)
export SSX3_SKIP_LOGOS="${SSX3_SKIP_LOGOS:-1}"        # skip the EA BIG and THX logos (SSX3_SKIP_INTRO=1: the intro too)
exec ./build/ssx3_recomp "$@"
