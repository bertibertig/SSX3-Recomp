#!/usr/bin/env bash
# Start-up menu for scripts/run.sh: internal resolution and frame rate (Vulkan renderer).
# Loaded with `source`; call launch_menu. Uses whiptail or dialog when installed, otherwise a built-in
# arrow-key menu. The last choice is saved and reused – also when there is no terminal (e.g. a Steam shortcut).
#   SSX3_MENU=0   never show the menu (saved choice / defaults only)
#   SSX3_MENU=1   show it even if SSX3_RES_SCALE / SSX3_FPS_LIMIT / SSX3_INTERP are already set
# Variables set in the environment win over the saved choice and are not asked for.

LAUNCH_CONF="${XDG_CONFIG_HOME:-$HOME/.config}/ssx3-recomp/run.conf"

RES_ITEMS=(
  1 "1x  640x480    native, fastest"
  2 "2x  1280x960"
  3 "3x  1920x1440"
  4 "4x  2560x1920"
  5 "5x  3200x2400  sharpest, heaviest"
)
FPS_ITEMS=(
  display  "Display refresh  interpolated up to the monitor's rate"
  60       "60 fps           the game's own frames, no interpolation"
  120      "120 fps          interpolated"
  144      "144 fps          interpolated"
  165      "165 fps          interpolated"
  240      "240 fps          interpolated"
  uncapped "Uncapped         interpolated, as fast as the GPU can"
)

# _menu_builtin <title> <prompt> <default tag> <tag> <text> ... -> chosen tag on stdout, 1 on cancel
_menu_builtin() {
  local title="$1" prompt="$2" def="$3"; shift 3
  local -a tags=() texts=()
  while [ $# -gt 0 ]; do tags+=("$1"); texts+=("$2"); shift 2; done
  local n=${#tags[@]} sel=0 i key rest width=0
  for i in "${!tags[@]}"; do
    [ "${tags[$i]}" = "$def" ] && sel=$i
    [ ${#texts[$i]} -gt "$width" ] && width=${#texts[$i]}
  done
  [ ${#prompt} -gt "$width" ] && width=${#prompt}
  width=$((width + 4))
  local hr; hr=$(printf '%*s' "$width" '' | tr ' ' '-')

  _menu_draw() {
    printf '\r\e[1m %s\e[0m\e[K\n' "$title"
    printf ' %s\e[K\n' "$hr"
    for i in "${!tags[@]}"; do
      if [ "$i" -eq "$sel" ]; then
        printf ' \e[7m > %-*s\e[0m\e[K\n' "$((width - 3))" "${texts[$i]}"
      else
        printf '    %-*s\e[K\n' "$((width - 3))" "${texts[$i]}"
      fi
    done
    printf ' %s\e[K\n' "$hr"
    printf ' \e[2m%s\e[0m\e[K\n' "$prompt"
  }
  local lines=$((n + 4))

  printf '\e[?25l' >/dev/tty                       # hide the cursor while the menu is up
  _menu_draw >/dev/tty
  while :; do
    IFS= read -rsn1 key </dev/tty || { printf '\e[?25h' >/dev/tty; return 1; }
    if [ "$key" = $'\e' ]; then
      rest=""
      IFS= read -rsn2 -t 0.05 rest </dev/tty || true
      key+="$rest"
    fi
    case "$key" in
      $'\e[A'|k) sel=$(((sel + n - 1) % n)) ;;
      $'\e[B'|j) sel=$(((sel + 1) % n)) ;;
      [1-9]) [ "$key" -le "$n" ] && sel=$((key - 1)) ;;
      "") break ;;                                 # Enter
      $'\e'|q|Q) printf '\e[?25h' >/dev/tty; return 1 ;;
    esac
    printf '\e[%dA' "$lines" >/dev/tty
    _menu_draw >/dev/tty
  done
  printf '\e[?25h\n' >/dev/tty
  printf '%s' "${tags[$sel]}"
}

# _menu <title> <prompt> <default tag> <tag> <text> ... -> chosen tag on stdout, 1 on cancel
_menu() {
  local title="$1" prompt="$2" def="$3"; shift 3
  local rows=$(($# / 2))
  case "$SSX3_MENU_TOOL" in
    whiptail|dialog)
      local notags=--notags; [ "$SSX3_MENU_TOOL" = dialog ] && notags=--no-tags
      "$SSX3_MENU_TOOL" $notags --backtitle "SSX 3 Recomp" --title "$title" --default-item "$def" \
        --menu "$prompt" $((rows + 8)) 72 "$rows" "$@" 3>&1 1>&2 2>&3 </dev/tty ;;
    *) _menu_builtin "$title" "Up/Down or 1-$rows, Enter = select, Esc/q = quit" "$def" "$@" ;;
  esac
}

_conf_get() {  # _conf_get KEY -> value from the saved choice (no `source`: the file is only data)
  [ -f "$LAUNCH_CONF" ] || return 0
  sed -n "s/^$1=\([A-Za-z0-9]*\)$/\1/p" "$LAUNCH_CONF" | tail -n1
}

_apply_fps() {
  case "$1" in
    display)  : ;;                                  # the back end's default: cap = fastest display's refresh
    60)       export SSX3_INTERP=0 ;;
    uncapped) export SSX3_FPS_LIMIT=0 ;;
    ''|*[!0-9]*) echo "run.sh: ignoring saved frame rate '$1'" >&2 ;;
    *)        export SSX3_FPS_LIMIT="$1" ;;
  esac
}

launch_menu() {
  [ "${SSX3_RENDERER:-vulkan}" = vulkan ] || return 0  # both settings only exist in the Vulkan back end

  local ask_res=1 ask_fps=1
  [ -n "${SSX3_RES_SCALE:-}" ] && ask_res=0
  [ -n "${SSX3_FPS_LIMIT:-}${SSX3_INTERP:-}" ] && ask_fps=0
  if [ "${SSX3_MENU:-}" = 1 ]; then ask_res=1; ask_fps=1; fi

  local res fps
  res="$(_conf_get RES_SCALE)"; res="${res:-${SSX3_RES_SCALE:-1}}"
  fps="$(_conf_get FPS)"; fps="${fps:-display}"

  local interactive=1
  { [ "${SSX3_MENU:-}" = 0 ] || [ ! -t 0 ] || [ ! -t 1 ]; } && interactive=0
  if [ "$interactive" = 1 ] && [ $((ask_res + ask_fps)) -gt 0 ]; then
    if [ -z "${SSX3_MENU_TOOL:-}" ]; then           # SSX3_MENU_TOOL=builtin|whiptail|dialog forces one
      SSX3_MENU_TOOL=builtin
      command -v dialog   >/dev/null 2>&1 && SSX3_MENU_TOOL=dialog
      command -v whiptail >/dev/null 2>&1 && SSX3_MENU_TOOL=whiptail
    fi
    if [ "$ask_res" = 1 ]; then
      res="$(_menu "Internal resolution" "Rendering resolution (F10 cycles it in game):" "$res" "${RES_ITEMS[@]}")" \
        || { echo "Cancelled." >&2; exit 0; }
    fi
    if [ "$ask_fps" = 1 ]; then
      fps="$(_menu "Frame rate" "The game runs at 60; higher rates are interpolated:" "$fps" "${FPS_ITEMS[@]}")" \
        || { echo "Cancelled." >&2; exit 0; }
    fi
    [ "$SSX3_MENU_TOOL" = builtin ] || clear
    mkdir -p "$(dirname "$LAUNCH_CONF")"
    printf '# scripts/run.sh – last start-up choice\nRES_SCALE=%s\nFPS=%s\n' "$res" "$fps" >"$LAUNCH_CONF"
  fi

  if [ "$ask_res" = 1 ]; then export SSX3_RES_SCALE="$res"; fi
  if [ "$ask_fps" = 1 ]; then unset SSX3_FPS_LIMIT SSX3_INTERP; _apply_fps "$fps"; fi
  local fps_text="display refresh"
  if [ "${SSX3_INTERP:-}" = 0 ]; then fps_text=60
  elif [ "${SSX3_FPS_LIMIT:-}" = 0 ]; then fps_text=uncapped
  elif [ -n "${SSX3_FPS_LIMIT:-}" ]; then fps_text="$SSX3_FPS_LIMIT"
  fi
  echo "run.sh: internal resolution ${SSX3_RES_SCALE:-1}x, frame rate $fps_text" >&2
}
