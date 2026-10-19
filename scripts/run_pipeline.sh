#!/usr/bin/env bash
# Runs the xboxrecomp pipeline (steps 2-5 of GETTING_STARTED.md).
#   scripts/run_pipeline.sh            # everything
#   scripts/run_pipeline.sh parse      # xbe_parser only
#   scripts/run_pipeline.sh disasm | names | funcid | abi | recomp
#   (names = apply Ghidra names from scripts/ghidra.sh; runs automatically with "all" once the export exists)
# Intermediate results go to xboxrecomp/tools/*/output/ (git ignores the dirty submodule).
source "$(dirname "$0")/_common.sh"
use_venv; need_xbe; apply_runtime_patches
STEP="${1:-all}"
cd "$TOOLKIT"
run() { echo; echo "=== $* ==="; "$@"; }

[[ "$STEP" =~ ^(all|parse)$  ]] && run python -m tools.xbe_parser "$XBE" --json "${XBE%.xbe}_analysis.json"   # name is mandatory: <xbe-stem>_analysis.json
# Seeds: functions the static analysis does not see (thread starts, ICALL targets from runtime logs,
# generated with tools.seed_from_log). Passed automatically once the file exists.
SEEDS="$ROOT/config/seed_functions.json"
SEED_ARGS=(); [ -f "$SEEDS" ] && SEED_ARGS=(--seed-functions "$SEEDS")
# The XDK libraries live in their own sections (D3D, D3DX, XGRPH, DSOUND, XPP). With --text-only alone they are
# not swept linearly, only reached via calls – functions get cut off in the middle (e.g. D3D device init
# 0x0023A840 → device creation fails, see docs/notes.md). DOLBY stays out: that is DSP code, not x86.
XDK_SECTIONS="D3D,D3DX,XGRPH,DSOUND,XPP"
[[ "$STEP" =~ ^(all|disasm)$ ]] && run python -m tools.disasm "$XBE" --text-only --extra-sections "$XDK_SECTIONS" -v "${SEED_ARGS[@]}"
# Write Ghidra names (FidDb/CRT + string labels) into functions.json – must be redone after every disasm,
# because disasm rewrites functions.json completely. Create the export with: scripts/ghidra.sh
GHIDRA_EXPORT="$TOOLKIT/tools/ghidra_naming/export/functions.json"
if [[ "$STEP" == names ]] || { [[ "$STEP" == all ]] && [ -f "$GHIDRA_EXPORT" ]; }; then
  run python tools/ghidra_naming/merge_names.py --apply
fi
[[ "$STEP" =~ ^(all|funcid)$ ]] && run python -m tools.func_id "$XBE" -v
[[ "$STEP" =~ ^(all|abi)$    ]] && run python -m tools.abi_analysis "$XBE" -v   # do NOT skip!
if [[ "$STEP" =~ ^(all|recomp)$ ]]; then
  mkdir -p "$GEN_DIR"
  # --exclude-manual: functions src/recomp_manual.c defines, wraps (sub_X_gen) or hooks on entry
  # (sub_X_enter, e.g. the widescreen camera hook) – the generated code must leave them to it.
  run python -m tools.recomp "$XBE" --all --split 250 --gen-dir "$GEN_DIR" --exclude-manual "$ROOT/src/recomp_manual.c"
fi
echo; echo "Done. Health report: scripts/doctor.sh"
