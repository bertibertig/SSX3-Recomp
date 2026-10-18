#!/usr/bin/env bash
# Startet den gebauten Port aus dem Repo-Root (Pfade in src/main.c sind relativ dazu).
#   scripts/run.sh                         # Ausgabe auf die Konsole
#   scripts/run.sh 2>&1 | tee logs/run.log # für doctor.sh / seed_from_log
# Laufzeit-Schalter (Details: docs/notes.md, xboxrecomp/docs):
#   RECOMP_TITLE_KEVENTS=1  SSX3 baut KEVENTs selbst (ohne: SIGSEGV in SetEvent unter POSIX) – Standard hier an
#   RECOMP_VBLANK=1         60-Hz-VBlank-Interrupt; D3D_BlockUntilVerticalBlank wartet darauf – Standard hier an
#   RECOMP_AC97_READY=1     AC'97-Codec bereit + emulierte APU; ohne scheitert DirectSoundCreate – Standard hier an
#   RECOMP_FB_WINDOW=1      Framebuffer-Fenster (SDL2)
#   RECOMP_PB_SCAN=1 / RECOMP_PB_EXEC=1   NV2A-Pushbuffer analysieren / ausführen
source "$(dirname "$0")/_common.sh"
[ -x "$ROOT/build/ssx3_recomp" ] || { echo "Kein Build. Erst: scripts/build.sh" >&2; exit 1; }
cd "$ROOT"
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS:-1}"
export RECOMP_VBLANK="${RECOMP_VBLANK:-1}"
export RECOMP_AC97_READY="${RECOMP_AC97_READY:-1}"
export RECOMP_PB_EXEC="${RECOMP_PB_EXEC:-1}"          # Pushbuffer ausführen (CPU-Rasterizer) – sonst kein Bild
export RECOMP_FB_WINDOW="${RECOMP_FB_WINDOW:-1}"      # Fenster mit dem Framebuffer
export RECOMP_USB="${RECOMP_USB:-1}"                  # emulierter USB-Controller (XPP/OHCI)
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD:-1}"        # Tastatur = Controller 1 (Enter=Start, Z=A, X=B, Pfeile=D-Pad …)
export SSX3_SKIP_LOGOS="${SSX3_SKIP_LOGOS:-1}"        # EA-BIG- und THX-Logo überspringen (SSX3_SKIP_INTRO=1: auch das Intro)
exec ./build/ssx3_recomp "$@"
