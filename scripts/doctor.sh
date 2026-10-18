#!/usr/bin/env bash
# Bring-up-Gesundheitsbericht (was ist noch ungelöst?). Optional: Laufzeit-Log als Argument.
#   scripts/doctor.sh [runtime.log]
source "$(dirname "$0")/_common.sh"
use_venv
cd "$TOOLKIT"
if [ -n "${1:-}" ]; then python tools/doctor.py --runtime-log "$1"; else python tools/doctor.py; fi
