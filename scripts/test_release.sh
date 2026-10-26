#!/usr/bin/env bash
# Tests the release packages in dist/ the way a player uses them: unpack, add game_files/, start the launcher.
#   scripts/test_release.sh v0.2.0                 # Linux + Windows (Wine)
#   SKIP_WINDOWS=1 scripts/test_release.sh v0.2.0  # SKIP_LINUX=1 likewise
# Results in logs/release-test-<version>/: summary.txt, *.log, frame dumps and contact sheets
# (linux_frames.png, windows_frames.png) to look at.
#
# Linux:   missing-game-files check; `run.sh --settings` in a pty (Enter, Enter: into the first category, then
#          save & drop in -> settings.ini), virtual pad: intro -> title -> menus -> race, Vulkan frame dumps.
# Windows: missing-game-files check through pwsh; the settings screen through `wine cmd /c start /wait /b
#          ssx3_recomp.exe --settings` (Enter, Enter; saves settings.ini in the prefix); the default flow (run.ps1, Vulkan through Wine) with the virtual pad, Vulkan frame dumps.
#          Needs pwsh and wine (binfmt_misc runs the .exe through Wine); own Wine prefix in the results folder.
#          Not `run.ps1 --settings`: from Linux pwsh the exe gets no Wine console, so the screen never shows.
# Needs: python3 with evdev + pillow, writable /dev/uinput. Settings go to the results folder (XDG_CONFIG_HOME,
# the Wine prefix), never to your own ~/.config. Your windows are not touched (no xdotool).
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
  if (cd "$P" && SSX3_INSTALL=0 ./run.sh) > "$OUT/linux_nofiles.log" 2>&1; then say "  FAIL: run.sh started without game files"
  else say "  missing game files: $(grep -m1 'default.xbe missing' "$OUT/linux_nofiles.log" >/dev/null && echo 'stops with the message (ok)' || echo 'FAIL, see linux_nofiles.log')"; fi
  say "  installer: $("$P/install_game.sh" --check "$GAME_FILES" 2>&1 | grep -a -o 'check: .*' || echo 'FAIL, no result')"
  link_game_files "$P"
  mkdir -p "$OUT/linux_dumps" "$OUT/linux_cfg"
  set +e
  XDG_CONFIG_HOME="$OUT/linux_cfg" SSX3_VK_DUMP="$OUT/linux_dumps/f" SSX3_VK_DUMP_EVERY=300 \
    python3 -I "$T/drive.py" --log "$OUT/linux.log" --seconds "$LINUX_SECS" --keys '\r' '\r' --pad linux \
    -- bash -c "cd '$P' && exec ./run.sh --settings"
  set -e
  say "  settings: $(plain "$OUT/linux.log" | grep -a -o '\[settings\].*' | head -1)"
  say "  saved: $(grep -v '^#' "$OUT/linux_cfg/ssx3-recomp/settings.ini" 2>/dev/null | tr '\n' ' ' || echo 'no settings.ini (FAIL)')"
  log_summary "$OUT/linux.log" linux
  ls "$OUT"/linux_dumps/*.bmp >/dev/null 2>&1 && python3 -I "$T/contact_sheet.py" "$OUT/linux_frames.png" "$OUT"/linux_dumps/*.bmp | tee -a "$SUMMARY"
fi

if [ "${SKIP_WINDOWS:-0}" != 1 ]; then
  command -v pwsh >/dev/null || { echo "pwsh (PowerShell 7) needed for the Windows test" >&2; exit 1; }
  command -v wine >/dev/null || { echo "wine needed for the Windows test" >&2; exit 1; }
  say "== Windows (Wine): $(basename "$DIST/SSX3-Recomp-$VERSION-windows-x86_64.zip")"
  P="$(unpack "$DIST/SSX3-Recomp-$VERSION-windows-x86_64.zip")"
  export WINEPREFIX="$OUT/wineprefix" WINEDEBUG=-all
  if SSX3_INSTALL=0 pwsh -NoProfile -File "$P/run.ps1" > "$OUT/win_nofiles.log" 2>&1 < /dev/null; then say "  FAIL: run.ps1 started without game files"
  else say "  missing game files: $(grep -q 'default.xbe missing' "$OUT/win_nofiles.log" && echo 'stops with the message (ok)' || echo 'FAIL, see win_nofiles.log')"; fi
  say "  installer: $(pwsh -NoProfile -File "$P/install_game.ps1" -Check "$GAME_FILES" 2>&1 < /dev/null | grep -a -o 'check: .*' || echo 'FAIL, no result')"
  link_game_files "$P"
  wineboot -i >/dev/null 2>&1 || true   # create the prefix before the settings screen starts
  set +e
  # the settings screen: Enter (into the first category), Enter (save & drop in), then stop. Started through
  # wine cmd so the GUI-subsystem exe has a Wine console on this pty (as run.ps1 gives it one on Windows).
  WIN_INI="$WINEPREFIX/drive_c/users/$USER/AppData/Roaming/ssx3-recomp/settings.ini"
  python3 -I "$T/drive.py" --log "$OUT/win_menu.log" --seconds 45 --key-delay 12 \
    --keys '\r' '\r' -- bash -c "cd '$P' && exec wine cmd /c start /wait /b ssx3_recomp.exe --settings"
  wineserver -k 2>/dev/null || true
  say "  settings: $(plain "$OUT/win_menu.log" | grep -a -o '\[settings\][^ ]*' | head -1)"
  say "  saved: $( [ -f "$WIN_INI" ] && grep -v '^#' "$WIN_INI" | tr -d '\r' | tr '\n' ' ' || echo 'no settings.ini (FAIL)')"
  # the default flow: run.ps1 with the saved settings (Vulkan), pad into a race
  mkdir -p "$OUT/win_dumps"
  SSX3_VK_DUMP="Z:$(echo "$OUT/win_dumps/w" | tr / '\\')" SSX3_VK_DUMP_EVERY=300 \
    python3 -I "$T/drive.py" --log "$OUT/windows.log" --seconds "$WIN_SECS" --pad windows \
    -- pwsh -NoProfile -File "$P/run.ps1"
  set -e
  wineserver -k 2>/dev/null || true
  log_summary "$OUT/windows.log" windows
  frames=("$OUT"/win_dumps/w0*.bmp); [ -f "$OUT/win_dumps/w" ] && frames+=("$OUT/win_dumps/w")
  [ -e "${frames[0]}" ] && python3 -I "$T/contact_sheet.py" "$OUT/windows_frames.png" "${frames[@]}" | tee -a "$SUMMARY"
fi

echo; echo "Results: $OUT (look at summary.txt and the *_frames.png contact sheets)"
