#!/usr/bin/env bash
# Bring-up health report (what is still unresolved?). Optional: runtime log as argument.
#   scripts/doctor.sh [runtime.log]
source "$(dirname "$0")/_common.sh"
use_venv
cd "$TOOLKIT"
if [ -n "${1:-}" ]; then python tools/doctor.py --runtime-log "$1"; else python tools/doctor.py; fi
