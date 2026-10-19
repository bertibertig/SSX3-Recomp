#!/usr/bin/env bash
# Tests the release packages in dist/ the way a player uses them: unpack, add game_files/, start the launcher.
#   scripts/test_release.sh v0.2.0                 # Linux + Windows (Wine)
#   SKIP_WINDOWS=1 scripts/test_release.sh v0.2.0  # SKIP_LINUX=1 likewise
# Results in logs/release-test-<version>/: summary.txt, *.log, frame dumps and contact sheets
# (linux_frames.png, windows_frames.png) to look at.
#
# Linux:   missing-game-files check; run.sh with the start-up menu forced on (SSX3_MENU=1, Enter x4) in a pty,
#          virtual pad: intro -> title -> menus -> race, Vulkan frame dumps.
# Windows: missing-game-files check through pwsh; run.ps1's start-up menu (SSX3_RENDERER=vulkan, saves run.conf);
#          the default flow (run.ps1, CPU renderer) under Wine with the virtual pad, CPU frame dumps.
#          Needs pwsh and wine (binfmt_misc runs the .exe through Wine); own Wine prefix in the results folder.
# Needs: python3 with evdev + pillow, writable /dev/uinput. Settings go to the results folder (XDG_CONFIG_HOME,
# APPDATA), never to your own ~/.config. Your windows are not touched (no xdotool).
source "$(dirname "$0")/_common.sh"
VERSION="${1:?Usage: scripts/test_release.sh <version, e.g. v0.2.0>}"
DIST="$ROOT/dist"
OUT="$ROOT/logs/release-test-$VERSION"
T="$ROOT/tests/release"
LINUX_SECS="${LINUX_SECS:-85}"
WIN_SECS="${WIN_SECS:-170}"
[ -f "$XBE" ] || { echo "game_files/default.xbe missing – the test needs your own game files" >&2; exit 1; }
python3 -I -c "import evdev, PIL" || { echo "python3 needs evdev and pillow" >&2; exit 1; }
[ -w /dev/uinput ] || { echo "/dev/uinput not writable (virtual pad)" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"; : > "$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }
plain() { sed 's/\x1b\[[0-9;?]*[A-Za-z]//g; s/\x1b[=>]//g' "$1" | tr -d '\r'; }   # log without terminal codes
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1

unpack() {  # unpack <archive> -> package dir on stdout
  local a="$1" d="$OUT/pkg"
  mkdir -p "$d"
  case "$a" in *.tar.gz) tar -C "$d" -xzf "$a" ;; *.zip) (cd "$d" && unzip -q -o "$a") ;; esac
  local p="$d/$(basename "${a%.tar.gz}")"; p="${p%.zip}"
  echo "$p"
}
link_game_files() {  # only what the disc has: default.xbe + data/ (not our analysis files)
  ln -sfn "$GAME_FILES/default.xbe" "$1/game_files/default.xbe"
  ln -sfn "$GAME_FILES/data" "$1/game_files/data"
}
log_summary() {  # log_summary <log> <label>
  local log="$1" lbl="$2"
  say "  $lbl: $(grep -a -m1 -o 'PAD: port [0-9] <- .*' "$log" | tr -d '\r' || echo 'no pad seen')"
  say "  $lbl: fps lines: $(grep -a -o -E '\[VK\] FPS: [0-9.]+ \(game [0-9.]+\)|\[GPU\] [0-9.]+ fps' "$log" | tail -4 | tr '\n' ';')"
  if grep -a -q -E '\[CRASH\]|Access violation|SIGSEGV' "$log"; then
    say "  $lbl: CRASH: $(grep -a -m1 -E '\[CRASH\]|Access violation|SIGSEGV' "$log" | tr -d '\r')"
  fi
  local ex; ex="$(grep -a -o '\[drive\] exit .*' "$log" | tail -1)"
  case "$ex" in "[drive] exit 0 "*) say "  $lbl: $ex (still running at the time limit = ok)" ;;
                *) say "  $lbl: $ex  <- CRASHED or quit early (139 = segfault)" ;; esac
}

if [ "${SKIP_LINUX:-0}" != 1 ]; then
  say "== Linux: $(basename "$DIST/SSX3-Recomp-$VERSION-linux-x86_64.tar.gz")"
  P="$(unpack "$DIST/SSX3-Recomp-$VERSION-linux-x86_64.tar.gz")"
  if (cd "$P" && ./run.sh) > "$OUT/linux_nofiles.log" 2>&1; then say "  FAIL: run.sh started without game files"
  else say "  missing game files: $(grep -m1 'default.xbe missing' "$OUT/linux_nofiles.log" >/dev/null && echo 'stops with the message (ok)' || echo 'FAIL, see linux_nofiles.log')"; fi
  link_game_files "$P"
  mkdir -p "$OUT/linux_dumps" "$OUT/linux_cfg"
  set +e
  XDG_CONFIG_HOME="$OUT/linux_cfg" SSX3_MENU=1 SSX3_VK_DUMP="$OUT/linux_dumps/f" SSX3_VK_DUMP_EVERY=300 \
    python3 -I "$T/drive.py" --log "$OUT/linux.log" --seconds "$LINUX_SECS" --keys '\r' '\r' '\r' '\r' --pad linux \
    -- bash -c "cd '$P' && exec ./run.sh"
  set -e
  say "  start-up menu: $(plain "$OUT/linux.log" | grep -a -o 'run.sh: Vulkan.*' | head -1)"
  say "  saved: $(tr '\n' ' ' < "$OUT/linux_cfg/ssx3-recomp/run.conf" 2>/dev/null || echo 'no run.conf (FAIL)')"
  log_summary "$OUT/linux.log" linux
  ls "$OUT"/linux_dumps/*.bmp >/dev/null 2>&1 && python3 -I "$T/contact_sheet.py" "$OUT/linux_frames.png" "$OUT"/linux_dumps/*.bmp | tee -a "$SUMMARY"
fi

if [ "${SKIP_WINDOWS:-0}" != 1 ]; then
  command -v pwsh >/dev/null || { echo "pwsh (PowerShell 7) needed for the Windows test" >&2; exit 1; }
  command -v wine >/dev/null || { echo "wine needed for the Windows test" >&2; exit 1; }
  say "== Windows (Wine): $(basename "$DIST/SSX3-Recomp-$VERSION-windows-x86_64.zip")"
  P="$(unpack "$DIST/SSX3-Recomp-$VERSION-windows-x86_64.zip")"
  export WINEPREFIX="$OUT/wineprefix" WINEDEBUG=-all APPDATA="$OUT/win_appdata"
  if pwsh -NoProfile -File "$P/run.ps1" > "$OUT/win_nofiles.log" 2>&1 < /dev/null; then say "  FAIL: run.ps1 started without game files"
  else say "  missing game files: $(grep -q 'default.xbe missing' "$OUT/win_nofiles.log" && echo 'stops with the message (ok)' || echo 'FAIL, see win_nofiles.log')"; fi
  link_game_files "$P"
  set +e
  # run.ps1's start-up menu: only offered for the Vulkan renderer; down + Enter, down + Enter, Enter
  SSX3_RENDERER=vulkan python3 -I "$T/drive.py" --log "$OUT/win_menu.log" --seconds 25 \
    --keys '\x1b[B' '\r' '\x1b[B' '\r' '\r' -- pwsh -NoProfile -File "$P/run.ps1"
  say "  start-up menu: $(plain "$OUT/win_menu.log" | grep -a -o 'run.ps1: Vulkan.*' | head -1)"
  say "  saved: $(grep -v '^#' "$OUT/win_appdata/ssx3-recomp/run.conf" 2>/dev/null | tr -d '\r' | tr '\n' ' ' || echo 'no run.conf (FAIL)')"
  # the default flow: run.ps1 without a menu (CPU renderer), pad into a race
  rm -f "$OUT/win_appdata/ssx3-recomp/run.conf"
  mkdir -p "$OUT/win_dumps"
  RECOMP_FB_DUMP="Z:$(echo "$OUT/win_dumps/w" | tr / '\\')" RECOMP_FB_WINDOW_DUMP_EVERY=150 \
    python3 -I "$T/drive.py" --log "$OUT/windows.log" --seconds "$WIN_SECS" --pad windows \
    -- pwsh -NoProfile -File "$P/run.ps1"
  set -e
  wineserver -k 2>/dev/null || true
  log_summary "$OUT/windows.log" windows
  frames=("$OUT"/win_dumps/w0*.bmp); [ -f "$OUT/win_dumps/w" ] && frames+=("$OUT/win_dumps/w")
  [ -e "${frames[0]}" ] && python3 -I "$T/contact_sheet.py" "$OUT/windows_frames.png" "${frames[@]}" | tee -a "$SUMMARY"
fi

echo; echo "Results: $OUT (look at summary.txt and the *_frames.png contact sheets)"
