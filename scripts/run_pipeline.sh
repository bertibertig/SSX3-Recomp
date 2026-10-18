#!/usr/bin/env bash
# Führt die xboxrecomp-Pipeline (Schritte 2-5 der GETTING_STARTED.md) aus.
#   scripts/run_pipeline.sh            # alles
#   scripts/run_pipeline.sh parse      # nur xbe_parser
#   scripts/run_pipeline.sh disasm | names | funcid | abi | recomp
#   (names = Ghidra-Namen aus scripts/ghidra.sh übernehmen; läuft bei "all" automatisch, sobald der Export existiert)
# Die Zwischenergebnisse landen in xboxrecomp/tools/*/output/ (git ignoriert das Submodul-Dirty).
source "$(dirname "$0")/_common.sh"
use_venv; need_xbe
STEP="${1:-all}"
cd "$TOOLKIT"
run() { echo; echo "=== $* ==="; "$@"; }

[[ "$STEP" =~ ^(all|parse)$  ]] && run python -m tools.xbe_parser "$XBE" --json "${XBE%.xbe}_analysis.json"   # Name ist Pflicht: <xbe-stem>_analysis.json
# Seeds: Funktionen, die die statische Analyse nicht sieht (Thread-Starts, ICALL-Ziele aus Laufzeit-Logs,
# erzeugt mit tools.seed_from_log). Werden automatisch mitgegeben, sobald die Datei existiert.
SEEDS="$ROOT/config/seed_functions.json"
SEED_ARGS=(); [ -f "$SEEDS" ] && SEED_ARGS=(--seed-functions "$SEEDS")
# Die XDK-Bibliotheken liegen in eigenen Sektionen (D3D, D3DX, XGRPH, DSOUND, XPP). Mit --text-only allein werden sie
# nicht linear durchsucht, nur über Calls angerissen – Funktionen werden mitten drin abgeschnitten (z. B. D3D-Device-Init
# 0x0023A840 → Device-Erstellung scheitert, siehe docs/notes.md). DOLBY bleibt draußen: das ist DSP-Code, kein x86.
XDK_SECTIONS="D3D,D3DX,XGRPH,DSOUND,XPP"
[[ "$STEP" =~ ^(all|disasm)$ ]] && run python -m tools.disasm "$XBE" --text-only --extra-sections "$XDK_SECTIONS" -v "${SEED_ARGS[@]}"
# Ghidra-Namen (FidDb/CRT + String-Labels) in functions.json schreiben – muss nach jedem disasm neu passieren,
# weil disasm functions.json komplett neu schreibt. Export erzeugen: scripts/ghidra.sh
GHIDRA_EXPORT="$TOOLKIT/tools/ghidra_naming/export/functions.json"
if [[ "$STEP" == names ]] || { [[ "$STEP" == all ]] && [ -f "$GHIDRA_EXPORT" ]; }; then
  run python tools/ghidra_naming/merge_names.py --apply
fi
[[ "$STEP" =~ ^(all|funcid)$ ]] && run python -m tools.func_id "$XBE" -v
[[ "$STEP" =~ ^(all|abi)$    ]] && run python -m tools.abi_analysis "$XBE" -v   # NICHT überspringen!
if [[ "$STEP" =~ ^(all|recomp)$ ]]; then
  mkdir -p "$GEN_DIR"
  run python -m tools.recomp "$XBE" --all --split 250 --gen-dir "$GEN_DIR"
fi
echo; echo "Fertig. Health-Report: scripts/doctor.sh"
