#!/usr/bin/env bash
# Starts the built port from the repo root or from an unpacked release folder (paths in src/main.c are relative
# to it).
#   scripts/run.sh                         # start with the saved settings
#   scripts/run.sh --settings              # settings screen first (resolution, aspect, frame rate, display,
#                                          # language, start-up); saved in ~/.config/ssx3-recomp/settings.ini
#   scripts/run.sh 2>&1 | tee logs/run.log # for doctor.sh / seed_from_log
# The SSX3_* variables below override the saved settings for one run (src/settings/settings.h).
# Runtime switches (details: docs/notes.md, xboxrecomp/docs):
#   RECOMP_TITLE_KEVENTS=1  SSX3 builds KEVENTs itself (without: SIGSEGV in SetEvent on POSIX) – on by default here
#   RECOMP_VBLANK=1         60 Hz VBlank interrupt; D3D_BlockUntilVerticalBlank waits for it – on by default here
#   RECOMP_AC97_READY=1     AC'97 codec ready + emulated APU; without it DirectSoundCreate fails – on by default here
#   SSX3_RENDERER=vulkan    GPU renderer (src/render, own window) – default
#   SSX3_RENDERER=cpu       the executor's software rasteriser + framebuffer window, kept as the reference
#                           (640x480, 4:3, 60 fps: none of the Vulkan-only switches below apply)
#   SSX3_LANGUAGE=<en|fr|de|es>  the game's language (default: the system's, if the disc has it)
#   SSX3_SETTINGS=<file>    another settings file
#   RECOMP_FB_WINDOW=1      framebuffer window (SDL2) – set automatically for SSX3_RENDERER=cpu
#   SSX3_VK_DUMP=<prefix> / RECOMP_FB_DUMP=<prefix>   BMP frame dumps (Vulkan / CPU) for comparisons
#   SSX3_INTERP=0           no frame interpolation (Vulkan): show only the game's own 60 fps frames
#   SSX3_FPS_LIMIT=<n>      interpolated frame cap (default: fastest display's refresh, 0 = uncapped)
#   SSX3_VSYNC=1            vsync (FIFO) instead of mailbox presentation
#   SSX3_RES_SCALE=<1..5>   internal resolution (Vulkan), multiple of 640x480; F10 cycles it in game
#   SSX3_ASPECT=<shape>     widescreen in game (Vulkan): 16:9, 16:10, 21:9, any W:H, auto (desktop); default 4:3
#   SSX3_WINDOW=<mode>      windowed (default), borderless or fullscreen (Vulkan); Alt+Enter / F11 toggle in game
#   SSX3_MONITOR=<m>        primary (default: the desktop's main monitor), a number or part of a name
#                           from `ssx3_recomp --list-displays`
#   RECOMP_PB_SCAN=1 / RECOMP_PB_EXEC=1   analyse / execute the NV2A pushbuffer
#   SSX3_PAD_TRACE=1        log every controller button change (pads: README "Controllers")
#   SSX3_PAD_SCRIPT=<file>  timed button script on port 1 for unattended test runs (src/input/ssx3_pad.c)
#   SSX3_VK_RC_REGS=<array|switch>  how the combiner shader holds its registers (default: switch on NVIDIA)
#   RECOMP_USB_PADS=2       more emulated pads on the USB bus (split screen), default 1
#   SSX3_INSTALL=0          without game_files/default.xbe: just stop instead of starting install_game.sh
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -x "$HERE/ssx3_recomp" ]; then
  set -euo pipefail                                   # release package: binary, scripts and game_files/ side by side
  ROOT="$HERE"; EXE="$ROOT/ssx3_recomp"
else
  source "$HERE/_common.sh"
  EXE="$ROOT/build/ssx3_recomp"
  [ -x "$EXE" ] || [ ! -x "$ROOT/build-docker/ssx3_recomp" ] || EXE="$ROOT/build-docker/ssx3_recomp"   # scripts/docker.sh build
fi
[ -x "$EXE" ] || { echo "No build. First run: scripts/build.sh" >&2; exit 1; }
if [ ! -f "$ROOT/game_files/default.xbe" ]; then
  echo "game_files/default.xbe missing: put the extracted contents of your SSX 3 Xbox disc (PAL) into $ROOT/game_files/" >&2
  [ "${SSX3_INSTALL:-1}" != 0 ] && [ -f "$HERE/install_game.sh" ] || exit 1
  echo "run.sh: starting the game data installer (install_game.sh; SSX3_INSTALL=0 skips it)" >&2
  rc=0; SSX3_INSTALL_FROM_RUN=1 bash "$HERE/install_game.sh" || rc=$?
  [ "$rc" = 10 ] && exit 0                            # installed, but the player chose not to start now
  [ "$rc" = 0 ] && [ -f "$ROOT/game_files/default.xbe" ] || exit "$((rc ? rc : 1))"
fi
cd "$ROOT"
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS:-1}"
export RECOMP_VBLANK="${RECOMP_VBLANK:-1}"
export RECOMP_AC97_READY="${RECOMP_AC97_READY:-1}"
export RECOMP_PB_EXEC="${RECOMP_PB_EXEC:-1}"          # decode/execute the pushbuffer – otherwise no picture
export RECOMP_USB="${RECOMP_USB:-1}"                  # emulated USB controller (XPP/OHCI)
# Renderer, window, resolution, language, keyboard, logos/intro: the saved settings (ssx3_recomp --settings).
exec "$EXE" "$@"
