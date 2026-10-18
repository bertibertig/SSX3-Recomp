#!/usr/bin/env bash
# Linux replacement for xboxrecomp/tools/ghidra_naming/run_ghidra.sh (which calls analyzeHeadless.bat,
# cygpath and `py -3` – Windows/Git Bash only). Same steps, same output locations, so that
# merge_names.py works unchanged:
#   1. extract_for_ghidra.py  → xboxrecomp/tools/ghidra_naming/work/xbe_flat.bin
#   2. analyzeHeadless: raw import x86:LE:32 @ 0x10000, auto-analysis (SetAnalysisOptions.java)
#   3. ExportXbeNames.py      → xboxrecomp/tools/ghidra_naming/export/{functions,symbols}.json
#
# Usage:
#   scripts/ghidra.sh                     # import + analyse + export
#   scripts/ghidra.sh decompile 4000      # additionally decompile up to 4000 functions
#   scripts/ghidra.sh decompile all       # everything (hours)
#   scripts/ghidra.sh merge [--apply]     # merge_names.py (→ ghidra_names.json, --apply writes functions.json)
#   scripts/ghidra.sh classify 0x22E280 … # ClassifyTargets.java against the finished project (read-only)
#   scripts/ghidra.sh dump [out.txt]      # DumpEntries.java (all Ghidra function entries)
#   scripts/ghidra.sh xrefs 0x24B0D8 …    # scripts/ghidra/XrefsTo.java: all references (read-only)
#   scripts/ghidra.sh decomp 0x23A840 …   # scripts/ghidra/Decomp.java: decompile a function (read-only)
#
# Env:
#   GHIDRA_HOME   (Default: /opt/ghidra)
#   IMPORT=0      program is already in the project → just open it
#   ANALYZE=0     no auto-analysis, only (re-)export / continue decompiling
#   GHIDRA_HEADLESS_MAXMEM  heap for Ghidra (default here: 8G; Ghidra itself: 2G)
#
# xboxrecomp's Ghidra scripts are Jython (`@runtime Jython`). Ghidra 12 only ships Jython
# as an extension zip; if it is missing, it is unpacked once into ~/.config/ghidra/<version>/Extensions.
source "$(dirname "$0")/_common.sh"
use_venv; need_xbe

GHIDRA_HOME="${GHIDRA_HOME:-/opt/ghidra}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
[ -x "$HEADLESS" ] || { echo "analyzeHeadless not found: $HEADLESS (set GHIDRA_HOME)" >&2; exit 1; }
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"

GN="$TOOLKIT/tools/ghidra_naming"
WORK="$GN/work"
PROJ_DIR="$WORK/ghidra_project"
PROJ_NAME="$(basename "$(dirname "$XBE")")"   # like run_ghidra.sh: folder of the XBE (= game_files)
EXPORT_DIR="$GN/export"
SCRIPT_DIR="$GN/ghidra_scripts"
OWN_SCRIPT_DIR="$ROOT/scripts/ghidra"   # our own query scripts
FLAT="$WORK/xbe_flat.bin"
PROG_NAME="xbe_flat.bin"
LOG="$WORK/analyze.log"

# --- Ensure the Jython extension --------------------------------------------------------------
ensure_jython() {
  local props ver rel user_ext zip
  props="$GHIDRA_HOME/Ghidra/application.properties"
  ver="$(sed -n 's/^application.version=//p' "$props")"
  rel="$(sed -n 's/^application.release.name=//p' "$props")"
  user_ext="${XDG_CONFIG_HOME:-$HOME/.config}/ghidra/ghidra_${ver}_${rel}/Extensions"
  [ -d "$GHIDRA_HOME/Ghidra/Extensions/Jython" ] || [ -d "$user_ext/Jython" ] && return 0
  zip="$(ls "$GHIDRA_HOME"/Extensions/Ghidra/ghidra_*_Jython.zip 2>/dev/null | head -1)"
  [ -n "$zip" ] || { echo "Jython extension missing and no zip under $GHIDRA_HOME/Extensions/Ghidra" >&2; exit 1; }
  echo "Installing Jython extension: $(basename "$zip") → $user_ext"
  mkdir -p "$user_ext"
  unzip -q "$zip" -d "$user_ext"
}

# Shared arguments for queries against the finished project (does not modify the project).
query() {
  [ -d "$PROJ_DIR/$PROJ_NAME.rep" ] || { echo "No Ghidra project – first run: scripts/ghidra.sh" >&2; exit 1; }
  "$HEADLESS" "$PROJ_DIR" "$PROJ_NAME" -process "$PROG_NAME" -readOnly -noanalysis \
    -scriptPath "$SCRIPT_DIR;$OWN_SCRIPT_DIR" -postScript "$@"
}

case "${1:-}" in
  merge)    shift; exec python "$GN/merge_names.py" "$@" ;;
  classify) shift; ensure_jython; query ClassifyTargets.java "$@"; exit ;;
  xrefs)    shift; ensure_jython; query XrefsTo.java "$@" | sed -n 's/.*\(XR .*\) (GhidraScript).*/\1/p'; exit ;;
  decomp)   shift; ensure_jython; query Decomp.java "$@" | sed -n '/DC =====/,$p' | grep -v '(HeadlessAnalyzer)\|^ERROR REPORT' \
              | sed 's/^INFO  Decomp.java> //; s/ (GhidraScript)\s*$//'; exit ;;
  dump)     shift; ensure_jython; query DumpEntries.java "$(realpath -m "${1:-$WORK/entries.txt}")"; exit ;;
esac

DO_DECOMPILE="${1:-nodecompile}"
DECOMP_LIMIT="${2:-4000}"
DECOMP_TIMEOUT="${3:-30}"
IMPORT="${IMPORT:-1}"
ANALYZE="${ANALYZE:-1}"

ensure_jython
mkdir -p "$WORK" "$EXPORT_DIR" "$PROJ_DIR"
echo "=== Ghidra-Naming: $XBE"
echo "    GHIDRA_HOME=$GHIDRA_HOME  Heap=$GHIDRA_HEADLESS_MAXMEM  Decompile=$DO_DECOMPILE ($DECOMP_LIMIT, ${DECOMP_TIMEOUT}s)"
echo "    Log: $LOG"

echo "[1/2] Building flat image from the XBE ..."
python "$GN/extract_for_ghidra.py" "$XBE" --out-dir "$WORK"

# No "-analysisTimeoutPerFile 0": in headless mode that means 0 seconds, not unlimited.
EXPORT_ARGS=(-scriptPath "$SCRIPT_DIR"
             -postScript ExportXbeNames.py "$EXPORT_DIR" "$DO_DECOMPILE" "$DECOMP_LIMIT" "$DECOMP_TIMEOUT")
if [ "$IMPORT" = "1" ]; then
  echo "[2/2] Import + analysis + export (takes a while) ..."
  NOANALYSIS=(); [ "$ANALYZE" = "0" ] && NOANALYSIS=(-noanalysis)
  "$HEADLESS" "$PROJ_DIR" "$PROJ_NAME" \
    -import "$FLAT" \
    -loader BinaryLoader \
    -loader-baseAddr 0x10000 \
    -processor "x86:LE:32:default" \
    -cspec windows \
    -overwrite \
    "${NOANALYSIS[@]}" \
    -preScript SetAnalysisOptions.java \
    "${EXPORT_ARGS[@]}" 2>&1 | tee "$LOG"
else
  echo "[2/2] Opening existing program + export ..."
  NOANALYSIS=(-noanalysis); [ "$ANALYZE" = "1" ] && NOANALYSIS=()
  "$HEADLESS" "$PROJ_DIR" "$PROJ_NAME" \
    -process "$PROG_NAME" \
    "${NOANALYSIS[@]}" \
    "${EXPORT_ARGS[@]}" 2>&1 | tee "$LOG"
fi

# Headless only reports script errors in the log; the exit code stays 0.
if grep -qE 'ERROR .*(Script|script)|Traceback' "$LOG"; then
  echo "WARNING: errors in the log – see $LOG" >&2
fi
echo "=== Done. Exports:"; ls -la "$EXPORT_DIR"
echo "Next step: scripts/ghidra.sh merge   (then: scripts/ghidra.sh merge --apply + re-run recomp)"
