#!/usr/bin/env bash
# Game data installer for SSX3-Recomp: puts the contents of YOUR OWN SSX 3 Xbox disc into game_files/ next to the
# port's binary, so a release folder is ready to play. Sources:
#   - a disc image: a full Redump-style .iso (video + game partition) or a trimmed/rebuilt .xiso
#     (extract-xiso -c, xdvdfs pack). The Xbox file system (XDVDFS) is read here, no extract-xiso / xdvdfs needed.
#   - an already extracted folder (the one with default.xbe in it): copied.
# Usage:
#   ./install_game.sh                        # full-screen menu (arrow keys); finds images/folders on its own
#   ./install_game.sh "SSX 3.iso"            # this source, still asks before writing
#   ./install_game.sh --yes "SSX 3.iso"      # no questions (scripts, tests)
#   ./install_game.sh --check [SOURCE]       # only identify the source (or the installed data), write nothing
#   ./install_game.sh --dest DIR SOURCE      # another target folder instead of game_files/
# Target: <folder of ssx3_recomp>/game_files in a release package, <repo>/game_files when run as scripts/install_game.sh.
#   NO_COLOR=1             no colours
#   SSX3_INSTALL_PLAIN=1   line-by-line output instead of the full-screen menu (also without a terminal)
# run.sh starts this on its own when game_files/default.xbe is missing (SSX3_INSTALL_FROM_RUN=1): "Ride now"
# then exits 0 and run.sh goes on with the launch; "Quit" after a finished install exits 10.
# Windows counterpart: install_game.ps1 / install_game.bat (same steps and look).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -e "$HERE/ssx3_recomp" ] || [ -e "$HERE/ssx3_recomp.exe" ]; then
  ROOT="$HERE"                                          # release package: binary, scripts and game_files/ side by side
elif [ -f "$HERE/_common.sh" ] && [ -d "$HERE/../src" ]; then
  ROOT="$(cd "$HERE/.." && pwd)"                        # repo checkout: scripts/install_game.sh
else
  ROOT="$HERE"
fi
DEST="$ROOT/game_files"

# The build the port was made from (docs/notes.md "Game version"): SSX 3 PAL/EU, title ID 0x4541003C
TITLE_ID=4541003C
SHA1_ORIG=ba876cc11ad38650eac9831b1c7f8c4dbf7c5ba9      # default.xbe as on the PAL disc
SHA1_PATCHED=409141774025fce02ab4743d1e669d35bb1d190e   # same build, region/media-free softmod patch
# Where the XDVDFS game partition starts: rebuilt .xiso, then Redump images of XGD1 (SSX 3), XGD2, XGD3 discs
PARTITION_OFFSETS=(0 $((0x18300000)) $((0x0FD90000)) $((0x02080000)))
XDVDFS_MAGIC="MICROSOFT*XBOX*MEDIA"
CHUNK=$((32 * 1024 * 1024))                             # copy granularity (progress updates)

YES=0; CHECK=0; SRC_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes)   YES=1 ;;
    --check)    CHECK=1 ;;
    --dest)     DEST="${2:?--dest needs a folder}"; shift ;;
    --dest=*)   DEST="${1#--dest=}" ;;
    -h|--help)  sed -n '2,/^set -uo/{/^set -uo/d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*)         echo "install_game: unknown option $1 (see --help)" >&2; exit 2 ;;
    *)          SRC_ARG="$1" ;;
  esac
  shift
done

# ---------------------------------------------------------------------------------------------------------------
# Started by double-click (no terminal) on a desktop: reopen in a terminal window
if [ ! -t 0 ] && [ -z "${SSX3_INSTALL_PLAIN:-}" ] && [ -z "$SRC_ARG" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] \
   && [ -z "${SSX3_INSTALL_RELAUNCHED:-}" ]; then
  export SSX3_INSTALL_RELAUNCHED=1
  self="$HERE/$(basename "${BASH_SOURCE[0]}")"
  for t in konsole gnome-terminal kgx xfce4-terminal x-terminal-emulator xterm; do
    command -v "$t" >/dev/null 2>&1 || continue
    case "$t" in
      gnome-terminal|kgx) exec "$t" -- bash "$self" ;;
      *)                  exec "$t" -e bash "$self" ;;
    esac
  done
fi

TUI=1
{ [ -n "${SSX3_INSTALL_PLAIN:-}" ] || [ ! -t 0 ] || [ ! -t 1 ] || [ "${TERM:-dumb}" = dumb ]; } && TUI=0
case "$(locale charmap 2>/dev/null)" in UTF-8|utf8) ;; *) TUI=0 ;; esac
[ "$YES" = 1 ] && [ -n "$SRC_ARG" ] && TUI=0
[ "$CHECK" = 1 ] && TUI=0

# ---------------------------------------------------------------------------------------------------------------
# Colours: SSX 3's menus – orange and sun-yellow on dark, ice-blue and snow-white for the mountain
COLOR=0
if [ -z "${NO_COLOR:-}" ] && [ -t 1 ] && [ "${TERM:-dumb}" != dumb ]; then
  COLOR=16
  [ "$(tput colors 2>/dev/null || echo 8)" -ge 256 ] && COLOR=256
  case "${COLORTERM:-}" in truecolor|24bit) COLOR=24 ;; esac
fi
# fg <r> <g> <b> <16-colour fallback code> -> escape sequence in REPLY
fg() {
  case "$COLOR" in
    24)  printf -v REPLY '\e[38;2;%d;%d;%dm' "$1" "$2" "$3" ;;
    256) printf -v REPLY '\e[38;5;%dm' $((16 + 36 * (($1 * 5 + 127) / 255) + 6 * (($2 * 5 + 127) / 255) + ($3 * 5 + 127) / 255)) ;;
    16)  printf -v REPLY '\e[%sm' "$4" ;;
    *)   REPLY="" ;;
  esac
}
bg() {
  case "$COLOR" in
    24)  printf -v REPLY '\e[48;2;%d;%d;%dm' "$1" "$2" "$3" ;;
    256) printf -v REPLY '\e[48;5;%dm' $((16 + 36 * (($1 * 5 + 127) / 255) + 6 * (($2 * 5 + 127) / 255) + ($3 * 5 + 127) / 255)) ;;
    16)  printf -v REPLY '\e[%sm' "$4" ;;
    *)   REPLY="" ;;
  esac
}
fg 255 122 0   33;   C_OR="$REPLY"      # SSX orange
fg 255 204 0   93;   C_YE="$REPLY"      # sun yellow
fg 120 210 255 96;   C_IC="$REPLY"      # ice blue
fg 70 120 170  34;   C_DP="$REPLY"      # deep blue (mountain shade)
fg 245 250 255 97;   C_SN="$REPLY"      # snow white
fg 130 140 155 90;   C_DIM="$REPLY"
fg 255 80 60   91;   C_RED="$REPLY"
fg 120 230 120 92;   C_GRN="$REPLY"
bg 255 122 0   43;   B_OR="$REPLY"
fg 20 20 30    30;   C_INK="$REPLY"     # text on the orange bar
if [ "$COLOR" = 0 ]; then B=""; IT=""; RST=""; else B=$'\e[1m'; IT=$'\e[3m'; RST=$'\e[0m'; fi
LOGO_COL=()                                             # orange -> yellow, top to bottom
for rgb in "255 84 0 31" "255 112 0 33" "255 140 0 33" "255 170 0 93" "255 200 20 93"; do
  # shellcheck disable=SC2086
  fg $rgb; LOGO_COL+=("$B$REPLY")
done

LOGO=(
'       ▄▄█████▄   ▄▄█████▄  ▀███▄   ▄███▀      ▄█████▄▄'
'      ███▀▀  ▀▀  ███▀▀  ▀▀    ▀███▄███▀        ▀▀  ▄██▀'
'      ▀▀█████▄   ▀▀█████▄       ▄███▄            ▀▀██▄'
'    ▄▄    ▄██▀ ▄▄    ▄██▀     ▄███▀███▄      ▄▄    ▄██▀'
'    ▀██████▀▀  ▀██████▀▀    ▄███▀   ▀███▄    ▀██████▀▀'
)
W=64                                                    # content width (measure() adapts it)
PEAK_NAME=("" "PICK YOUR LINE" "CHECK THE GEAR" "DROP IN")

# The three peaks of the mountain, two text rows high (16 levels); the peak of the current step is lit orange
RIDGE_HEIGHT=(0 10 13 16)
ridge_row() {  # ridge_row <0 top|1 bottom> <lit peak 0..3> -> REPLY
  local row=$1 lit=$2 x p h best owner lvl cell out="" cur="" col
  local -a RIDGE_CENTER=(0 $((W * 18 / 100)) $((W * 47 / 100)) $((W * 78 / 100)))
  local blocks=(" " "▁" "▂" "▃" "▄" "▅" "▆" "▇" "█")
  for ((x = 0; x < W; x++)); do
    best=0; owner=0
    for p in 1 2 3; do
      h=$((RIDGE_HEIGHT[p] - ((x > RIDGE_CENTER[p] ? x - RIDGE_CENTER[p] : RIDGE_CENTER[p] - x) * 5 + 2) / 4))
      [ "$h" -gt "$best" ] && { best=$h; owner=$p; }
    done
    [ "$best" -lt 1 ] && best=1
    if [ "$row" = 0 ]; then lvl=$((best - 8)); else lvl=$best; fi
    [ "$lvl" -lt 0 ] && lvl=0; [ "$lvl" -gt 8 ] && lvl=8
    cell="${blocks[$lvl]}"
    if [ "$owner" = "$lit" ]; then col="$C_OR"
    elif [ "$row" = 0 ]; then col="$C_SN"
    else col="$C_DP"; fi
    [ "$col" != "$cur" ] && { out+="$col"; cur="$col"; }
    out+="$cell"
  done
  REPLY="$out$RST"
}

# ---------------------------------------------------------------------------------------------------------------
# Screen
COLS=80; ROWS=24; M=2
measure() {
  local s; s="$(stty size </dev/tty 2>/dev/null)" || s="24 80"
  ROWS="${s% *}"; COLS="${s#* }"
  W=$((COLS - 6)); [ "$W" -gt 80 ] && W=80; [ "$W" -lt 40 ] && W=40   # content width
  M=$(((COLS - W) / 2)); [ "$M" -lt 1 ] && M=1
}
padr() {  # padr <width> <text> -> REPLY, padded with spaces by characters (printf pads by bytes)
  local s=$2 n=$(($1 - ${#2}))
  [ "$n" -gt 0 ] && printf -v s '%s%*s' "$2" "$n" ''
  REPLY="$s"
}
rule() {  # rule <width> -> REPLY, a line of ─
  printf -v REPLY '%*s' "$1" ''; REPLY="${REPLY// /─}"
}
line() { printf '%*s%s\e[K\n' "$M" '' "$*"; }           # one screen line, left margin, rest of the row cleared
center() {  # center <visible width> <text with escapes>
  local w=$1; shift; printf '%*s%s\e[K\n' $((M + (W - w) / 2 > 0 ? M + (W - w) / 2 : 0)) '' "$*"
}
fit() {  # fit <width> <text> -> REPLY, shortened in the middle with …
  local w=$1 s=$2
  if [ ${#s} -le "$w" ]; then REPLY="$s"; else REPLY="${s:0:$((w / 2 - 1))}…${s: -$((w - w / 2))}"; fi
}

tui_on()  { printf '\e[?1049h\e[?25l\e[H\e[2J'; }
tui_off() { printf '\e[?25h\e[?1049l'; }
cleanup() { [ "$TUI" = 1 ] && tui_off; }
trap cleanup EXIT
trap 'cleanup; TUI=0; echo; echo "install_game: aborted. Run it again to finish the copy." >&2; exit 130' INT TERM

header() {  # header <current peak 0..4 (4 = all done)>
  local step=$1 i lit=$1 trail="" s mark col
  measure
  printf '\e[H'
  line ""
  if [ "$COLS" -ge $((W + 2)) ] && [ "$ROWS" -ge 30 ]; then
    for i in 0 1 2 3 4; do line "${LOGO_COL[$i]}${IT}${LOGO[$i]}$RST"; done
    [ "$step" -ge 4 ] && lit=0
    ridge_row 0 "$lit"; line "$REPLY"
    ridge_row 1 "$lit"; line "$REPLY"
  else
    line "${LOGO_COL[1]}${B}${IT}SSX 3$RST"
  fi
  center 33 "${C_IC}R E C O M P${RST}   ${C_DIM}·${RST}   ${C_SN}game data setup$RST"
  line ""
  for i in 1 2 3; do
    if [ "$i" -lt "$step" ]; then mark="◆"; col="$C_GRN"
    elif [ "$i" -eq "$step" ]; then mark="◆"; col="$C_OR$B"
    else mark="◇"; col="$C_DIM"; fi
    s="$col$mark PEAK $i$RST"
    [ "$i" -eq "$step" ] && s+=" ${C_SN}${B}${IT}${PEAK_NAME[$i]}$RST"
    trail+="$s"
    [ "$i" -lt 3 ] && trail+="  ${C_DIM}━━━${RST}  "
  done
  local tw=$(( 3 * 8 + 2 * 7 + (step >= 1 && step <= 3 ? ${#PEAK_NAME[$step]} + 1 : 0) ))
  center "$tw" "$trail"
  line ""
}
footer() {  # footer <hint> – bottom row, then clear what was below the content
  printf '\e[J\e[%d;1H' "$ROWS"
  line "${C_DIM}$*$RST"
}
panel() {  # panel <title> – an orange slanted section title
  rule $((W - ${#1} - 4))
  line "${C_OR}${B}${IT}▶▶ $1$RST ${C_DIM}$REPLY$RST"
}
kv() { padr 12 "$1"; line "  ${C_DIM}$REPLY$RST ${C_SN}$2$RST"; }

read_key() {  # -> KEY: up, down, enter, esc, or the character
  local k rest
  IFS= read -rsn1 k </dev/tty || { KEY=esc; return; }
  if [ "$k" = $'\e' ]; then
    rest=""; IFS= read -rsn2 -t 0.05 rest </dev/tty || true
    case "$rest" in '[A'|OA) KEY=up ;; '[B'|OB) KEY=down ;; '') KEY=esc ;; *) KEY=other ;; esac
  elif [ -z "$k" ]; then KEY=enter
  else KEY="$k"; fi
}

# menu <peak> <title> <default tag> <tag> <label> <detail> ... -> MENU (chosen tag), returns 1 on Esc/q.
# A <preamble> function, if MENU_PRE is set, draws extra lines between the title and the list.
MENU_PRE=""
menu() {
  local peak=$1 title=$2 def=$3; shift 3
  local -a tags=() labels=() details=()
  while [ $# -gt 0 ]; do tags+=("$1"); labels+=("$2"); details+=("$3"); shift 3; done
  local n=${#tags[@]} sel=0 i lw=0
  for i in "${!tags[@]}"; do
    [ "${tags[$i]}" = "$def" ] && sel=$i
    [ ${#labels[$i]} -gt "$lw" ] && lw=${#labels[$i]}
  done
  while :; do
    header "$peak"
    panel "$title"
    [ -n "$MENU_PRE" ] && "$MENU_PRE"
    line ""
    local lab det
    for i in "${!tags[@]}"; do
      fit $((W - lw - 7)) "${details[$i]}"; det=$REPLY
      padr "$lw" "${labels[$i]}"; lab=$REPLY
      if [ "$i" -eq "$sel" ]; then
        line "${C_OR}${B}▶${RST} ${B_OR}${C_INK}${B}${IT} $lab ${RST} ${C_YE}$det$RST"
      else
        line "   ${C_SN}$lab ${RST}  ${C_DIM}$det$RST"
      fi
    done
    footer "↑↓ choose    ⏎ select    esc quit"
    read_key
    case "$KEY" in
      up|k)   sel=$(((sel + n - 1) % n)) ;;
      down|j) sel=$(((sel + 1) % n)) ;;
      [1-9])  [ "$KEY" -le "$n" ] && sel=$((KEY - 1)) ;;
      enter)  MENU="${tags[$sel]}"; return 0 ;;
      esc|q|Q) return 1 ;;
    esac
  done
}

# message <peak> <title> <colour> <line> ... – a screen with text, waits for a key
message() {
  local peak=$1 title=$2 col=$3; shift 3
  header "$peak"; panel "$title"; line ""
  local l; for l in "$@"; do line "  $col$l$RST"; done
  footer "⏎ continue"
  read_key
}

say() {  # plain-mode output
  [ "$TUI" = 1 ] && return 0
  printf '%s\n' "$*"
}
die() {
  if [ "$TUI" = 1 ]; then message 0 "WIPEOUT" "$C_RED" "$@"; fi
  TUI=0; cleanup; trap - EXIT
  printf 'install_game: %s\n' "$@" >&2
  exit 1
}

human() {  # human <bytes> -> REPLY
  local b=$1
  if   [ "$b" -ge $((1 << 30)) ]; then REPLY="$(awk -v b="$b" 'BEGIN{printf "%.2f GB", b/1073741824}')"
  elif [ "$b" -ge $((1 << 20)) ]; then REPLY="$(awk -v b="$b" 'BEGIN{printf "%.1f MB", b/1048576}')"
  else REPLY="$((b / 1024)) KB"; fi
}

# ---------------------------------------------------------------------------------------------------------------
# Reading the source. Results: SRC_KIND (image|folder), SRC_DESC, and the file list
#   FILES[i] relative path, FSRC[i] file to read, FOFF[i] byte offset in it, FSIZE[i] size; DIRS[] relative dirs
le32() { REPLY=$(( BYTES[$1] | BYTES[$1 + 1] << 8 | BYTES[$1 + 2] << 16 | BYTES[$1 + 3] << 24 )); }
le16() { REPLY=$(( BYTES[$1] | BYTES[$1 + 1] << 8 )); }
load_bytes() {  # load_bytes <file> <offset> <count> -> BYTES[] (decimal), 1 if short
  # shellcheck disable=SC2207
  BYTES=($(od -An -v -tu1 -j "$2" -N "$3" -- "$1" 2>/dev/null))
  [ "${#BYTES[@]}" -eq "$3" ]
}
fsize() { stat -Lc %s -- "$1" 2>/dev/null || echo 0; }

# xdvdfs_probe <image> -> PART (byte offset of the game partition), ROOT_SECT, ROOT_SIZE; 1 if not an Xbox image
xdvdfs_probe() {
  local img=$1 off size magic
  size=$(fsize "$img")
  for off in "${PARTITION_OFFSETS[@]}"; do
    [ $((off + 0x10000 + 2048)) -le "$size" ] || continue
    magic="$(dd if="$img" bs=20 count=1 skip=$((off + 0x10000)) iflag=skip_bytes status=none 2>/dev/null | tr -d '\0')"
    [ "$magic" = "$XDVDFS_MAGIC" ] || continue
    load_bytes "$img" $((off + 0x10000 + 20)) 8 || continue
    le32 0; ROOT_SECT=$REPLY; le32 4; ROOT_SIZE=$REPLY
    PART=$off
    return 0
  done
  return 1
}

# xdvdfs_dir <image> <sector> <size> <relative dir> – walks one directory's binary tree, recurses into subdirs
xdvdfs_dir() {
  local img=$1 sect=$2 size=$3 rel=$4
  [ "$size" -gt 0 ] || return 0
  [ "$size" -le $((16 << 20)) ] || { BAD="directory table too large in /$rel"; return 1; }
  [ $((PART + sect * 2048 + size)) -le "$IMG_SIZE" ] || { BAD="directory /$rel lies outside the image"; return 1; }
  load_bytes "$img" $((PART + sect * 2048)) "$size" || { BAD="cannot read directory /$rel"; return 1; }
  local -a stack=(0) subs=()
  local -A seen=()
  local o l r es fs at nl name i c
  while [ ${#stack[@]} -gt 0 ]; do
    o=${stack[-1]}; unset 'stack[-1]'
    [ -n "${seen[$o]:-}" ] && continue
    seen[$o]=1
    [ $((o + 14)) -le "$size" ] || continue
    le16 "$o"; l=$REPLY; le16 $((o + 2)); r=$REPLY
    [ "$l" = 65535 ] && [ "$r" = 65535 ] && continue      # sector padding
    le32 $((o + 4)); es=$REPLY; le32 $((o + 8)); fs=$REPLY
    at=${BYTES[$((o + 12))]}; nl=${BYTES[$((o + 13))]}
    [ "$nl" -gt 0 ] && [ $((o + 14 + nl)) -le "$size" ] || { BAD="broken entry in /$rel"; return 1; }
    name=""
    for ((i = o + 14; i < o + 14 + nl; i++)); do
      c=${BYTES[$i]}
      [ "$c" -ge 32 ] && [ "$c" -le 126 ] && [ "$c" != 47 ] && [ "$c" != 92 ] || { BAD="bad file name in /$rel"; return 1; }
      printf -v c "\\$(printf '%03o' "$c")"; name+="$c"
    done
    case "$name" in .|..) BAD="bad file name in /$rel"; return 1 ;; esac
    if [ $((at & 0x10)) -ne 0 ]; then
      subs+=("$es" "$fs" "$name"); DIRS+=("$rel$name")
    else
      [ $((PART + es * 2048 + fs)) -le "$IMG_SIZE" ] || { BAD="/$rel$name lies outside the image (incomplete download?)"; return 1; }
      FILES+=("$rel$name"); FSRC+=("$img"); FOFF+=($((PART + es * 2048))); FSIZE+=("$fs"); TOTAL=$((TOTAL + fs))
    fi
    [ "$l" -ne 0 ] && stack+=($((l * 4)))
    [ "$r" -ne 0 ] && stack+=($((r * 4)))
  done
  for ((i = 0; i < ${#subs[@]}; i += 3)); do
    DEPTH=$((DEPTH + 1)); [ "$DEPTH" -le 32 ] || { BAD="directories nested too deep"; return 1; }
    xdvdfs_dir "$img" "${subs[$i]}" "${subs[$((i + 1))]}" "$rel${subs[$((i + 2))]}/" || return 1
    DEPTH=$((DEPTH - 1))
  done
}

# scan_source <path> -> file list + SRC_KIND/SRC_DESC/SRC; 1 with BAD set if unusable
scan_source() {
  local p=$1 sub
  FILES=(); FSRC=(); FOFF=(); FSIZE=(); DIRS=(); TOTAL=0; BAD=""; DEPTH=0
  [ -e "$p" ] || { BAD="not found: $p"; return 1; }
  p="$(realpath -- "$p")"
  if [ -d "$p" ]; then
    if [ ! -f "$p/default.xbe" ]; then                     # a parent folder with exactly one game folder in it
      sub="$(find "$p" -mindepth 2 -maxdepth 2 -name default.xbe -type f 2>/dev/null | head -2)"
      [ -n "$sub" ] && [ "$(printf '%s\n' "$sub" | wc -l)" = 1 ] && p="$(dirname "$sub")"
    fi
    [ -f "$p/default.xbe" ] || { BAD="no default.xbe in this folder – pick the folder of the extracted disc"; return 1; }
    local d; d="$(realpath -m -- "$DEST")"
    if [ "$CHECK" = 0 ]; then
      [ "$p" != "$d" ] || { BAD="this is already the target folder"; return 1; }
      case "$d/" in "$p"/*) BAD="the target folder lies inside the source folder"; return 1 ;; esac
    fi
    SRC_KIND=folder; SRC_DESC="extracted folder"; SRC="$p"
    local type size rel
    while IFS=$'\t' read -r -d '' type size rel; do
      case "$type" in
        d) DIRS+=("$rel") ;;
        f) FILES+=("$rel"); FSRC+=("$p/$rel"); FOFF+=(0); FSIZE+=("$size"); TOTAL=$((TOTAL + size)) ;;
      esac
    done < <(find -L "$p" -mindepth 1 \( -type d -o -type f \) -printf '%y\t%s\t%P\0' 2>/dev/null)
  else
    SRC="$p"; IMG_SIZE=$(fsize "$p")
    xdvdfs_probe "$p" || { BAD="not an Xbox disc image (no XDVDFS file system found)"; return 1; }
    SRC_KIND=image
    if [ "$PART" = 0 ]; then SRC_DESC="Xbox image (.xiso)"; else SRC_DESC="full disc image (Redump .iso)"; fi
    xdvdfs_dir "$p" "$ROOT_SECT" "$ROOT_SIZE" "" || return 1
  fi
  [ "${#FILES[@]}" -gt 0 ] || { BAD="the source is empty"; return 1; }
  return 0
}

# identify_xbe – reads default.xbe of the scanned source: XBE_TITLE, XBE_NAME, XBE_REGION, XBE_SHA1 and
# VERDICT: ok (known build) | pal (SSX 3 PAL, unknown hash) | region (SSX 3, other region) | other | bad
identify_xbe() {
  local i idx=-1 base cert c region
  XBE_TITLE=""; XBE_NAME=""; XBE_REGION=""; XBE_SHA1=""; VERDICT=bad
  for i in "${!FILES[@]}"; do
    [ "${FILES[$i],,}" = default.xbe ] && { idx=$i; break; }
  done
  [ "$idx" -ge 0 ] || return 0
  load_bytes "${FSRC[$idx]}" "${FOFF[$idx]}" 8192 || return 0
  [ "${BYTES[0]}${BYTES[1]}${BYTES[2]}${BYTES[3]}" = 88666972 ] || return 0      # "XBEH"
  le32 $((0x104)); base=$REPLY; le32 $((0x118)); cert=$((REPLY - base))
  [ "$cert" -ge 0 ] && [ $((cert + 0xA4)) -le 8192 ] || return 0
  le32 $((cert + 8)); printf -v XBE_TITLE '%08X' "$REPLY"
  for ((i = cert + 12; i < cert + 12 + 80; i += 2)); do
    c=${BYTES[$i]}; [ "$c" = 0 ] && break
    [ "$c" -ge 32 ] && [ "$c" -le 126 ] && { printf -v c "\\$(printf '%03o' "$c")"; XBE_NAME+="$c"; }
  done
  le32 $((cert + 0xA0)); region=$REPLY
  XBE_REGION=""
  [ $((region & 1)) -ne 0 ] && XBE_REGION+="NTSC-U "
  [ $((region & 2)) -ne 0 ] && XBE_REGION+="NTSC-J "
  [ $((region & 4)) -ne 0 ] && XBE_REGION+="PAL "
  XBE_REGION="${XBE_REGION% }"; [ "$region" = 7 ] && XBE_REGION="region-free"
  XBE_SHA1="$(dd if="${FSRC[$idx]}" bs=4M skip="${FOFF[$idx]}" count="${FSIZE[$idx]}" \
              iflag=skip_bytes,count_bytes status=none 2>/dev/null | sha1sum | cut -c1-40)"
  if [ "$XBE_SHA1" = "$SHA1_ORIG" ] || [ "$XBE_SHA1" = "$SHA1_PATCHED" ]; then VERDICT=ok
  elif [ "$XBE_TITLE" != "$TITLE_ID" ]; then VERDICT=other
  elif [ $((region & 4)) -ne 0 ]; then VERDICT=pal
  else VERDICT=region; fi
}

verdict_text() {  # -> REPLY (one line), REPLY2 (colour)
  case "$VERDICT" in
    ok)     REPLY="SSX 3 · PAL · the exact build this port was made from ✓"; REPLY2="$C_GRN" ;;
    pal)    REPLY="SSX 3 · PAL · modified default.xbe (e.g. media patch), should work"; REPLY2="$C_YE" ;;
    region) REPLY="SSX 3 · $XBE_REGION – only the PAL version is supported"; REPLY2="$C_RED" ;;
    other)  REPLY="not SSX 3: ${XBE_NAME:-?} (title ID $XBE_TITLE)"; REPLY2="$C_RED" ;;
    *)      REPLY="no readable default.xbe"; REPLY2="$C_RED" ;;
  esac
}

# ---------------------------------------------------------------------------------------------------------------
# Finding sources on this machine: disc images (.iso/.xiso > 100 MB) and folders with an SSX 3 default.xbe
find_candidates() {
  CANDS=()
  local -a roots=("$HERE" "$ROOT" "$(dirname "$ROOT")" "$HOME" "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents"
                  "$HOME/Games" "$HOME/ROMs" "$HOME/Emulation/roms/xbox")
  local m; for m in /run/media/"${USER:-x}"/* /media/"${USER:-x}"/*; do [ -d "$m" ] && roots+=("$m"); done
  local -A seen=()
  local r f d dest
  dest="$(realpath -m -- "$DEST")"
  for r in "${roots[@]}"; do
    [ -d "$r" ] || continue
    while IFS= read -r -d '' f; do
      f="$(realpath -- "$f" 2>/dev/null)" || continue
      [ -n "${seen[$f]:-}" ] && continue; seen[$f]=1
      xdvdfs_probe "$f" && CANDS+=("$f")                  # other games' / PC .iso files are skipped
    done < <(find "$r" -maxdepth 2 -type f \( -iname '*.iso' -o -iname '*.xiso' \) -size +100M -print0 2>/dev/null)
    while IFS= read -r -d '' f; do
      d="$(realpath -- "$(dirname "$f")" 2>/dev/null)" || continue
      [ "$d" = "$dest" ] && continue
      [ -n "${seen[$d]:-}" ] && continue; seen[$d]=1
      CANDS+=("$d")
    done < <(find "$r" -maxdepth 3 -type f -name default.xbe -size +1M -print0 2>/dev/null)
    [ "${#CANDS[@]}" -ge 12 ] && break
  done
}

# ---------------------------------------------------------------------------------------------------------------
# Copying with a progress bar ("boost meter")
draw_progress() {  # draw_progress <done bytes> <file index>
  local got=$1 idx=$2 pct cells i bar="" now el speed eta d t
  pct=$((TOTAL > 0 ? got * 100 / TOTAL : 100))
  if [ "$TUI" = 0 ]; then
    [ "$pct" -ge "$NEXT_PCT" ] && { printf '  %3d%%  %s\n' "$pct" "${FILES[$idx]}"; NEXT_PCT=$((pct + 10)); }
    return
  fi
  local bw=$((W - 12))
  cells=$((TOTAL > 0 ? got * bw * 8 / TOTAL : bw * 8))
  local part=("" "▏" "▎" "▍" "▌" "▋" "▊" "▉")
  for ((i = 0; i < bw; i++)); do
    if [ $((cells / 8)) -gt "$i" ]; then
      if   [ $((i * 3)) -lt "$bw" ];     then bar+="$C_OR█"
      elif [ $((i * 3)) -lt $((bw * 2)) ]; then bar+="${LOGO_COL[3]}█"
      else bar+="$C_YE█"; fi
    elif [ $((cells / 8)) -eq "$i" ] && [ $((cells % 8)) -gt 0 ]; then bar+="$C_YE${part[$((cells % 8))]}"
    else bar+="$C_DIM·"; fi
  done
  now=$(date +%s); el=$((now - T0)); [ "$el" -lt 1 ] && el=1
  speed=$((got / el)); eta="--:--"
  [ "$speed" -gt 0 ] && { t=$(((TOTAL - got) / speed)); printf -v eta '%d:%02d' $((t / 60)) $((t % 60)); }
  human "$got"; d=$REPLY; human "$TOTAL"; t=$REPLY
  printf '\e[%d;1H' "$PROG_ROW"
  line "  ${C_DIM}BOOST$RST $bar$RST ${C_SN}${B}$(printf '%3d%%' "$pct")$RST"
  line ""
  fit $((W - 16)) "${FILES[$idx]}"
  kv "carving" "$REPLY"
  kv "distance" "$d of $t"
  human "$speed"; kv "speed" "$REPLY/s    ${C_DIM}ETA$RST ${C_SN}$eta"
}

install_files() {
  local i pos n got=0 dst last=0 now
  T0=$(date +%s); NEXT_PCT=0
  if [ "$TUI" = 1 ]; then
    header 3; panel "DROP IN"
    line ""
    line "  ${C_SN}Copying ${#FILES[@]} files to$RST ${C_IC}$(fit $((W - 24)) "$DEST"; echo "$REPLY")$RST"
    line ""
    measure; PROG_ROW=$(( $(header_rows) + 5 ))
    footer "ctrl+c abort (run it again to finish)"
  else
    say "Copying ${#FILES[@]} files ($(human "$TOTAL"; echo "$REPLY")) to $DEST"
  fi
  mkdir -p "$DEST" || die "cannot create $DEST"
  for i in "${!DIRS[@]}"; do mkdir -p "$DEST/${DIRS[$i]}" || die "cannot create $DEST/${DIRS[$i]}"; done
  for i in "${!FILES[@]}"; do
    dst="$DEST/${FILES[$i]}"
    mkdir -p "$(dirname "$dst")" && : > "$dst" || die "cannot write $dst"
    pos=0
    while [ "$pos" -lt "${FSIZE[$i]}" ]; do
      n=$((FSIZE[i] - pos)); [ "$n" -gt "$CHUNK" ] && n=$CHUNK
      dd if="${FSRC[$i]}" of="$dst" bs=4M iflag=skip_bytes,count_bytes,fullblock oflag=seek_bytes conv=notrunc \
         skip=$((FOFF[i] + pos)) seek="$pos" count="$n" status=none 2>/dev/null || die "read/write error on ${FILES[$i]}"
      pos=$((pos + n)); got=$((got + n))
      now=$(date +%s%N); [ $((now - last)) -ge 100000000 ] && { draw_progress "$got" "$i"; last=$now; }
    done
    [ "$(fsize "$dst")" = "${FSIZE[$i]}" ] || die "size mismatch after copying ${FILES[$i]} (disk full?)"
    [ "${FSIZE[$i]}" = 0 ] && draw_progress "$got" "$i"
  done
  NEXT_PCT=0; draw_progress "$TOTAL" $((${#FILES[@]} - 1))
  ELAPSED=$(($(date +%s) - T0))
}
header_rows() {  # rows the header takes at the current size (see header)
  if [ "$COLS" -ge $((W + 2)) ] && [ "$ROWS" -ge 30 ]; then echo 12; else echo 6; fi
}

# ---------------------------------------------------------------------------------------------------------------
# Steps
existing_info() {  # what is installed in DEST already -> EXIST_TEXT ("" if nothing)
  EXIST_TEXT=""
  [ -f "$DEST/default.xbe" ] || return 0
  local sha; sha="$(sha1sum "$DEST/default.xbe" 2>/dev/null | cut -c1-40)"
  if [ "$sha" = "$SHA1_ORIG" ] || [ "$sha" = "$SHA1_PATCHED" ]; then EXIST_TEXT="SSX 3 PAL is already installed ✓"
  else EXIST_TEXT="game_files/ already holds a default.xbe (unknown build)"; fi
}

pick_source() {  # interactive: -> SRC_PICK
  local -a items=()
  local c i kind lab
  find_candidates
  for i in "${!CANDS[@]}"; do
    c="${CANDS[$i]}"
    if [ -d "$c" ]; then kind="folder"
    else
      xdvdfs_probe "$c"; human "$(fsize "$c")"; kind="$REPLY "
      if [ "$PART" = 0 ]; then kind+="xiso"; else kind+="disc image"; fi
    fi
    lab="$(basename "$c")"; fit 26 "$lab"; lab=$REPLY
    c="$(dirname "$c")"; case "$c" in "$HOME"*) c="~${c#"$HOME"}" ;; esac
    items+=("c$i" "$lab" "$kind  in $c")
  done
  items+=(path "Type a path…" "tab completes, drag & drop works")
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && { command -v zenity >/dev/null || command -v kdialog >/dev/null; }; then
    items+=(bimg "Browse for an image…" "file dialog: .iso / .xiso")
    items+=(bdir "Browse for a folder…" "file dialog: the extracted disc folder")
  fi
  items+=(quit "Quit" "")
  MENU_PRE=pick_pre
  menu 1 "PICK YOUR LINE" "${LAST_PICK:-c0}" "${items[@]}" || { MENU_PRE=""; return 1; }
  MENU_PRE=""
  LAST_PICK="$MENU"
  case "$MENU" in
    quit) return 1 ;;
    c*)   SRC_PICK="${CANDS[${MENU#c}]}" ;;
    path)
      header 1; panel "PICK YOUR LINE"; line ""
      line "  ${C_SN}Path to the disc image (.iso / .xiso) or to the extracted folder:$RST"
      line "  ${C_DIM}tab completes · drag & drop a file here · empty = back$RST"
      line ""; printf '\e[J'
      printf '%*s%s' "$M" '' "  ${C_OR}${B}▶$RST "
      printf '\e[?25h'
      local p=""; IFS= read -e -r p </dev/tty || p=""
      printf '\e[?25l'
      p="${p#"${p%%[![:space:]]*}"}"; p="${p%"${p##*[![:space:]]}"}"      # trim
      case "$p" in \'*\') p="${p:1:${#p}-2}" ;; \"*\") p="${p:1:${#p}-2}" ;; esac
      case "$p" in "~"|"~/"*) p="$HOME${p:1}" ;; esac
      case "$p" in file://*) p="${p#file://}"; p="$(printf '%b' "${p//%/\\x}")" ;; esac
      [ -e "$p" ] || p="${p//\\ / }"                                      # shell-escaped spaces
      [ -n "$p" ] || { SRC_PICK=""; return 0; }
      SRC_PICK="$p" ;;
    bimg|bdir)
      local p=""
      if command -v zenity >/dev/null; then
        if [ "$MENU" = bimg ]; then p="$(zenity --file-selection --title="SSX 3 disc image" \
              --file-filter="Xbox disc images | *.iso *.ISO *.xiso *.XISO" --file-filter="All files | *" 2>/dev/null)"
        else p="$(zenity --file-selection --directory --title="Extracted SSX 3 disc folder" 2>/dev/null)"; fi
      else
        if [ "$MENU" = bimg ]; then p="$(kdialog --title "SSX 3 disc image" --getopenfilename "$HOME" \
              "*.iso *.ISO *.xiso *.XISO|Xbox disc images" 2>/dev/null)"
        else p="$(kdialog --title "Extracted SSX 3 disc folder" --getexistingdirectory "$HOME" 2>/dev/null)"; fi
      fi
      SRC_PICK="$p" ;;
  esac
}
pick_pre() {
  line ""
  line "  ${C_SN}Where is your SSX 3 disc? A disc image or an extracted folder.$RST"
  fit $((W - 12)) "$DEST"; line "  ${C_DIM}target  $REPLY$RST"
  [ -n "$EXIST_TEXT" ] && line "  ${C_GRN}$EXIST_TEXT$RST"
  [ "${#CANDS[@]}" -gt 0 ] || line "  ${C_DIM}(nothing found in the usual places – type a path or browse)$RST"
}

gear_pre() {
  line ""
  fit $((W - 16)) "$SRC"
  kv "source" "$REPLY"
  kv "type" "$SRC_DESC"
  human "$TOTAL"; kv "contents" "${#FILES[@]} files · $REPLY"
  kv "game" "${XBE_NAME:-?}  ${C_DIM}title ID ${XBE_TITLE:-?} · ${XBE_REGION:-?}"
  verdict_text; local v=$REPLY; padr 12 check; line "  ${C_DIM}$REPLY$RST $REPLY2$v$RST"
  fit $((W - 16)) "$DEST"
  kv "target" "$REPLY"
  human "$FREE"; local f=$REPLY
  if [ "$FREE" -lt "$TOTAL" ]; then kv "free space" "${C_RED}$f – not enough room"
  else kv "free space" "$f"; fi
  [ -n "$EXIST_TEXT" ] && line "  ${C_YE}$EXIST_TEXT – its files get overwritten$RST"
}

free_space() {
  local d="$DEST"; while [ ! -d "$d" ]; do d="$(dirname "$d")"; done
  FREE=$(df -B1 --output=avail -- "$d" 2>/dev/null | tail -1 | tr -d ' ')
  [ -n "$FREE" ] || FREE=0
}

finish_screen() {
  local t; printf -v t '%d:%02d' $((ELAPSED / 60)) $((ELAPSED % 60))
  local -a items=()
  { [ -x "$HERE/run.sh" ] || [ -n "${SSX3_INSTALL_FROM_RUN:-}" ]; } && items+=(run "Ride now" "start SSX 3")
  items+=(quit "Quit" "start it later with ./run.sh")
  MENU_PRE=finish_pre; FIN_T=$t
  menu 4 "PEAK CONQUERED" run "${items[@]}" || MENU=quit
  MENU_PRE=""
}
finish_pre() {
  line ""
  center 43 "${C_YE}${B}${IT}✦  G A M E   D A T A   I N S T A L L E D  ✦$RST"
  line ""
  human "$TOTAL"; kv "copied" "${#FILES[@]} files · $REPLY in $FIN_T"
  fit $((W - 16)) "$DEST"; kv "to" "$REPLY"
}

# ---------------------------------------------------------------------------------------------------------------
main_plain() {
  local src="$SRC_ARG"
  if [ "$CHECK" = 1 ] && [ -z "$src" ]; then src="$DEST"; fi
  [ -n "$src" ] || die "no source given. Usage: install_game.sh [--yes] <SSX 3 .iso | .xiso | extracted folder>"
  say "SSX3-Recomp game data setup"
  say "[1/3] Source: $src"
  scan_source "$src" || die "$BAD"
  identify_xbe
  human "$TOTAL"
  say "[2/3] $SRC_DESC, ${#FILES[@]} files, $REPLY"
  verdict_text
  say "      game: ${XBE_NAME:-?} (title ID ${XBE_TITLE:-?}, ${XBE_REGION:-?}), default.xbe SHA-1 ${XBE_SHA1:-?}"
  say "      check: $REPLY"
  [ "$CHECK" = 1 ] && { [ "$VERDICT" = ok ] || [ "$VERDICT" = pal ]; exit $?; }
  case "$VERDICT" in ok|pal) ;; *) [ "$YES" = 1 ] || die "not the supported game version (see above); --yes copies anyway" ;; esac
  free_space; [ "$FREE" -ge "$TOTAL" ] || { human "$FREE"; die "not enough free space at $DEST ($REPLY free)"; }
  existing_info; [ -n "$EXIST_TEXT" ] && say "      target: $EXIST_TEXT – overwriting"
  if [ "$YES" != 1 ]; then
    [ -t 0 ] || die "no terminal to confirm; add --yes"
    read -r -p "Copy to $DEST? [Y/n] " a; case "$a" in ""|y|Y|yes) ;; *) exit 1 ;; esac
  fi
  say "[3/3] Installing"
  install_files
  say "Done in ${ELAPSED}s. Start the game with ./run.sh"
}

main_tui() {
  tui_on
  existing_info
  [ -n "$SRC_ARG" ] && SRC_PICK="$SRC_ARG"
  while :; do
    if [ -z "$SRC_ARG" ]; then
      pick_source || exit 1
      [ -n "$SRC_PICK" ] || continue
    fi
    SRC_ARG=""
    header 2; panel "CHECK THE GEAR"; line ""; line "  ${C_SN}Reading $(fit $((W - 12)) "$SRC_PICK"; echo "$REPLY") …$RST"; printf '\e[J'
    if ! scan_source "$SRC_PICK"; then
      message 1 "WIPEOUT" "$C_RED" "$BAD" "" "${C_DIM}Pick another source."; continue
    fi
    identify_xbe; free_space
    local -a items=()
    case "$VERDICT" in
      ok|pal) items+=(go "Drop in" "copy the game data") ;;
      *)      items+=(go "Copy anyway" "this is not the supported SSX 3 PAL disc") ;;
    esac
    items+=(back "Pick another source" "" quit "Quit" "")
    MENU_PRE=gear_pre
    menu 2 "CHECK THE GEAR" "$( [ "$VERDICT" = ok ] || [ "$VERDICT" = pal ] && echo go || echo back)" "${items[@]}" || exit 1
    MENU_PRE=""
    case "$MENU" in quit) exit 1 ;; back) continue ;; esac
    if [ "$FREE" -lt "$TOTAL" ]; then
      human "$FREE"; message 2 "WIPEOUT" "$C_RED" "Not enough free space at the target ($REPLY free)."; continue
    fi
    install_files
    finish_screen
    if [ -n "${SSX3_INSTALL_FROM_RUN:-}" ]; then           # run.sh waits for us and launches the game itself
      [ "$MENU" = run ] && exit 0
      exit 10
    fi
    if [ "$MENU" = run ]; then
      tui_off; trap - EXIT
      cd "$ROOT" && exec "$HERE/run.sh"
    fi
    exit 0
  done
}

if [ "$TUI" = 1 ]; then main_tui; else main_plain; fi
