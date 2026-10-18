#!/usr/bin/env bash
# Linux-Ersatz für xboxrecomp/tools/ghidra_naming/run_ghidra.sh (das ruft analyzeHeadless.bat,
# cygpath und `py -3` auf – nur Windows/Git-Bash). Gleiche Schritte, gleiche Ausgabeorte, damit
# merge_names.py unverändert funktioniert:
#   1. extract_for_ghidra.py  → xboxrecomp/tools/ghidra_naming/work/xbe_flat.bin
#   2. analyzeHeadless: Raw-Import x86:LE:32 @ 0x10000, Auto-Analyse (SetAnalysisOptions.java)
#   3. ExportXbeNames.py      → xboxrecomp/tools/ghidra_naming/export/{functions,symbols}.json
#
# Aufruf:
#   scripts/ghidra.sh                     # importieren + analysieren + exportieren
#   scripts/ghidra.sh decompile 4000      # zusätzlich bis zu 4000 Funktionen dekompilieren
#   scripts/ghidra.sh decompile all       # alles (Stunden)
#   scripts/ghidra.sh merge [--apply]     # merge_names.py (→ ghidra_names.json, --apply schreibt functions.json)
#   scripts/ghidra.sh classify 0x22E280 … # ClassifyTargets.java gegen das fertige Projekt (read-only)
#   scripts/ghidra.sh dump [out.txt]      # DumpEntries.java (alle Ghidra-Funktionseinträge)
#   scripts/ghidra.sh xrefs 0x24B0D8 …    # scripts/ghidra/XrefsTo.java: alle Referenzen (read-only)
#   scripts/ghidra.sh decomp 0x23A840 …   # scripts/ghidra/Decomp.java: Funktion dekompilieren (read-only)
#
# Env:
#   GHIDRA_HOME   (Default: /opt/ghidra)
#   IMPORT=0      Programm ist schon im Projekt → nur öffnen
#   ANALYZE=0     keine Auto-Analyse, nur (erneut) exportieren / weiter dekompilieren
#   GHIDRA_HEADLESS_MAXMEM  Heap für Ghidra (Default hier: 8G; Ghidra selbst: 2G)
#
# Die Ghidra-Skripte von xboxrecomp sind Jython (`@runtime Jython`). Ghidra 12 liefert Jython nur noch
# als Extension-Zip aus; fehlt sie, wird sie einmalig nach ~/.config/ghidra/<version>/Extensions entpackt.
source "$(dirname "$0")/_common.sh"
use_venv; need_xbe

GHIDRA_HOME="${GHIDRA_HOME:-/opt/ghidra}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
[ -x "$HEADLESS" ] || { echo "analyzeHeadless nicht gefunden: $HEADLESS (GHIDRA_HOME setzen)" >&2; exit 1; }
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"

GN="$TOOLKIT/tools/ghidra_naming"
WORK="$GN/work"
PROJ_DIR="$WORK/ghidra_project"
PROJ_NAME="$(basename "$(dirname "$XBE")")"   # wie run_ghidra.sh: Ordner der XBE (= game_files)
EXPORT_DIR="$GN/export"
SCRIPT_DIR="$GN/ghidra_scripts"
OWN_SCRIPT_DIR="$ROOT/scripts/ghidra"   # eigene Abfrage-Skripte
FLAT="$WORK/xbe_flat.bin"
PROG_NAME="xbe_flat.bin"
LOG="$WORK/analyze.log"

# --- Jython-Extension sicherstellen ------------------------------------------------------------
ensure_jython() {
  local props ver rel user_ext zip
  props="$GHIDRA_HOME/Ghidra/application.properties"
  ver="$(sed -n 's/^application.version=//p' "$props")"
  rel="$(sed -n 's/^application.release.name=//p' "$props")"
  user_ext="${XDG_CONFIG_HOME:-$HOME/.config}/ghidra/ghidra_${ver}_${rel}/Extensions"
  [ -d "$GHIDRA_HOME/Ghidra/Extensions/Jython" ] || [ -d "$user_ext/Jython" ] && return 0
  zip="$(ls "$GHIDRA_HOME"/Extensions/Ghidra/ghidra_*_Jython.zip 2>/dev/null | head -1)"
  [ -n "$zip" ] || { echo "Jython-Extension fehlt und kein Zip unter $GHIDRA_HOME/Extensions/Ghidra" >&2; exit 1; }
  echo "Installiere Jython-Extension: $(basename "$zip") → $user_ext"
  mkdir -p "$user_ext"
  unzip -q "$zip" -d "$user_ext"
}

# Gemeinsame Argumente für Abfragen gegen das fertige Projekt (ändert nichts am Projekt).
query() {
  [ -d "$PROJ_DIR/$PROJ_NAME.rep" ] || { echo "Kein Ghidra-Projekt – erst: scripts/ghidra.sh" >&2; exit 1; }
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

echo "[1/2] Flat-Image aus der XBE bauen ..."
python "$GN/extract_for_ghidra.py" "$XBE" --out-dir "$WORK"

# Kein "-analysisTimeoutPerFile 0": headless heißt das 0 Sekunden, nicht unbegrenzt.
EXPORT_ARGS=(-scriptPath "$SCRIPT_DIR"
             -postScript ExportXbeNames.py "$EXPORT_DIR" "$DO_DECOMPILE" "$DECOMP_LIMIT" "$DECOMP_TIMEOUT")
if [ "$IMPORT" = "1" ]; then
  echo "[2/2] Import + Analyse + Export (dauert) ..."
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
  echo "[2/2] Vorhandenes Programm öffnen + Export ..."
  NOANALYSIS=(-noanalysis); [ "$ANALYZE" = "1" ] && NOANALYSIS=()
  "$HEADLESS" "$PROJ_DIR" "$PROJ_NAME" \
    -process "$PROG_NAME" \
    "${NOANALYSIS[@]}" \
    "${EXPORT_ARGS[@]}" 2>&1 | tee "$LOG"
fi

# Headless meldet Skriptfehler nur im Log, Exit-Code bleibt 0.
if grep -qE 'ERROR .*(Script|script)|Traceback' "$LOG"; then
  echo "WARNUNG: Fehler im Log – siehe $LOG" >&2
fi
echo "=== Fertig. Exporte:"; ls -la "$EXPORT_DIR"
echo "Nächster Schritt: scripts/ghidra.sh merge   (dann: scripts/ghidra.sh merge --apply + recomp neu)"
