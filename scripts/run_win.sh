#!/usr/bin/env bash
# Startet build-win/ssx3_recomp.exe unter Proton (umu-run), Arbeitsverzeichnis = Repo-Root (Pfade in src/main.c).
#   scripts/run_win.sh                       # Ausgabe danach auf die Konsole
#   scripts/run_win.sh > logs/win.log        # zum Auswerten
# Muster: xboxrecomp/tests/proton_run.sh – über eine Batch-Datei, weil umu-run stderr des Programms verwirft.
# Env: WINEPREFIX (Default ~/.local/share/ssx3-recomp-proton), PROTONPATH (Default GE-Proton),
#      RUN_TIMEOUT (Sekunden, Default 60), RECOMP_* werden durchgereicht (Defaults wie scripts/run.sh).
source "$(dirname "$0")/_common.sh"
EXE="$ROOT/build-win/ssx3_recomp.exe"
[ -f "$EXE" ] || { echo "Kein Windows-Build. Erst: scripts/build_win.sh" >&2; exit 1; }
command -v umu-run >/dev/null || { echo "umu-run fehlt" >&2; exit 1; }
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS:-1}"
export RECOMP_VBLANK="${RECOMP_VBLANK:-1}"
export RECOMP_AC97_READY="${RECOMP_AC97_READY:-1}"
export RECOMP_PB_EXEC="${RECOMP_PB_EXEC:-1}"          # Pushbuffer ausführen (CPU-Rasterizer) – sonst kein Bild
export RECOMP_FB_WINDOW="${RECOMP_FB_WINDOW:-1}"      # Fenster mit dem Framebuffer
export RECOMP_USB="${RECOMP_USB:-1}"                  # emulierter USB-Controller (XPP/OHCI)
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD:-1}"        # Tastatur = Controller 1 (Enter=Start, Z=A, X=B, Pfeile=D-Pad …)
export SSX3_SKIP_LOGOS="${SSX3_SKIP_LOGOS:-1}"        # EA-BIG- und THX-Logo überspringen (SSX3_SKIP_INTRO=1: auch das Intro)

winpath() { local p="Z:$1"; echo "${p//\//\\}"; }
out=$(mktemp "${TMPDIR:-/tmp}/ssx3_win.XXXXXX"); bat=$out.cmd
cmdline="\"$(winpath "$EXE")\""
for a in "$@"; do cmdline="$cmdline \"${a//%/%%}\""; done
printf '@echo off\r\ncd /d "%s"\r\n%s > "%s" 2>&1\r\necho run_win_exit=%%ERRORLEVEL%%>> "%s"\r\n' \
    "$(winpath "$ROOT")" "$cmdline" "$(winpath "$out")" "$(winpath "$out")" > "$bat"

envs=(WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/ssx3-recomp-proton}"
      PROTONPATH="${PROTONPATH:-GE-Proton}" GAMEID="${GAMEID:-umu-default}")
while IFS= read -r kv; do envs+=("$kv"); done < <(env | grep -E '^RECOMP_' || true)
cd "$ROOT"
env "${envs[@]}" timeout -k 10 -s INT "${RUN_TIMEOUT:-60}" umu-run cmd.exe /c "$(winpath "$bat")" </dev/null >"$out.umu" 2>&1 || true
rc=$(grep -a -o 'run_win_exit=[0-9-]*' "$out" 2>/dev/null | tail -1 | cut -d= -f2 || true)
grep -a -v 'run_win_exit=' "$out" 2>/dev/null || true
[ -n "$rc" ] || { echo "--- umu-run-Ausgabe (letzte 15 Zeilen):"; tail -15 "$out.umu"; }
rm -f "$out" "$bat" "$out.umu"
[ -n "$rc" ] || { echo "run_win: kein Exit-Code (Timeout oder Start fehlgeschlagen)"; exit 125; }
echo "run_win: exit $rc"
