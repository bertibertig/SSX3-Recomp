#!/usr/bin/env bash
# Starts build-win/ssx3_recomp.exe under Proton (umu-run), working directory = repo root (paths in src/main.c).
#   scripts/run_win.sh                       # output to the console afterwards
#   scripts/run_win.sh > logs/win.log        # for analysis
# Pattern: xboxrecomp/tests/proton_run.sh – via a batch file, because umu-run discards the program's stderr.
# Env: WINEPREFIX (Default ~/.local/share/ssx3-recomp-proton), PROTONPATH (Default GE-Proton),
#      RUN_TIMEOUT (seconds, default 60), RECOMP_* and SSX3_* are passed through (defaults as in scripts/run.sh;
#      SSX3_RENDERER=vulkan by default, =cpu for the software rasteriser).
source "$(dirname "$0")/_common.sh"
EXE="$ROOT/build-win/ssx3_recomp.exe"
[ -f "$EXE" ] || { echo "No Windows build. First run: scripts/build_win.sh" >&2; exit 1; }
command -v umu-run >/dev/null || { echo "umu-run missing" >&2; exit 1; }
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS:-1}"
export RECOMP_VBLANK="${RECOMP_VBLANK:-1}"
export RECOMP_AC97_READY="${RECOMP_AC97_READY:-1}"
export RECOMP_PB_EXEC="${RECOMP_PB_EXEC:-1}"          # execute the pushbuffer (CPU rasteriser) – otherwise no picture
export SSX3_RENDERER="${SSX3_RENDERER:-vulkan}"
if [ "$SSX3_RENDERER" = cpu ]; then
  export RECOMP_FB_WINDOW="${RECOMP_FB_WINDOW:-1}"    # the CPU rasteriser's window
else
  unset RECOMP_FB_WINDOW                              # the Vulkan back end opens its own window
fi
export RECOMP_USB="${RECOMP_USB:-1}"                  # emulated USB controller (XPP/OHCI)
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD:-1}"        # keyboard = controller 1 (Enter=Start, Z=A, X=B, arrows=D-pad …)
export SSX3_SKIP_LOGOS="${SSX3_SKIP_LOGOS:-1}"        # skip the EA BIG and THX logos (SSX3_SKIP_INTRO=1: the intro too)

winpath() { local p="Z:$1"; echo "${p//\//\\}"; }
out=$(mktemp "${TMPDIR:-/tmp}/ssx3_win.XXXXXX"); bat=$out.cmd
cmdline="\"$(winpath "$EXE")\""
for a in "$@"; do cmdline="$cmdline \"${a//%/%%}\""; done
printf '@echo off\r\ncd /d "%s"\r\n%s > "%s" 2>&1\r\necho run_win_exit=%%ERRORLEVEL%%>> "%s"\r\n' \
    "$(winpath "$ROOT")" "$cmdline" "$(winpath "$out")" "$(winpath "$out")" > "$bat"

envs=(WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/ssx3-recomp-proton}"
      PROTONPATH="${PROTONPATH:-GE-Proton}" GAMEID="${GAMEID:-umu-default}")
while IFS= read -r kv; do envs+=("$kv"); done < <(env | grep -E '^(RECOMP|SSX3)_' || true)
cd "$ROOT"
env "${envs[@]}" timeout -k 10 -s INT "${RUN_TIMEOUT:-60}" umu-run cmd.exe /c "$(winpath "$bat")" </dev/null >"$out.umu" 2>&1 || true
rc=$(grep -a -o 'run_win_exit=[0-9-]*' "$out" 2>/dev/null | tail -1 | cut -d= -f2 || true)
grep -a -v 'run_win_exit=' "$out" 2>/dev/null || true
[ -n "$rc" ] || { echo "--- umu-run output (last 15 lines):"; tail -15 "$out.umu"; }
rm -f "$out" "$bat" "$out.umu"
[ -n "$rc" ] || { echo "run_win: no exit code (timeout or start failed)"; exit 125; }
echo "run_win: exit $rc"
