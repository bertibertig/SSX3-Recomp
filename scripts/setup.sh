#!/usr/bin/env bash
# Sets up the Python environment for the xboxrecomp pipeline and checks build tools.
#   scripts/setup.sh
source "$(dirname "$0")/_common.sh"

if [ ! -f "$TOOLKIT/CMakeLists.txt" ]; then
  echo "xboxrecomp submodule missing, fetching it ..."
  git -C "$ROOT" submodule update --init --recursive
fi

if [ ! -d "$VENV" ]; then
  python3 -m venv "$VENV"
fi
use_venv
python -m pip install --upgrade pip >/dev/null
python -m pip install -r "$ROOT/requirements.txt"
python -c "import capstone; print('capstone', capstone.__version__)"

echo
echo "== Checking tools =="
for t in git python3 cmake gcc; do
  printf '  %-10s' "$t"; command -v "$t" >/dev/null && echo ok || echo "MISSING"
done
printf '  %-10s' "ISO-Extraktor"
if command -v extract-xiso >/dev/null || command -v xdvdfs >/dev/null; then echo ok
else echo "MISSING (install extract-xiso or xdvdfs)"; fi

if [ "$(uname -s)" = "Linux" ]; then
  echo
  echo "For the Linux build (SDL2/OpenGL/OpenSSL/ffmpeg) run once:"
  echo "  bash xboxrecomp/tools/linux/install_deps.sh     # needs sudo"
fi
echo
echo "Done. Next step: scripts/extract_iso.sh <SSX3-Xbox.iso>"
