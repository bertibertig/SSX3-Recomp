#!/usr/bin/env bash
# Richtet die Python-Umgebung für die xboxrecomp-Pipeline ein und prüft Build-Tools.
#   scripts/setup.sh
source "$(dirname "$0")/_common.sh"

if [ ! -f "$TOOLKIT/CMakeLists.txt" ]; then
  echo "xboxrecomp-Submodul fehlt, hole es ..."
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
echo "== Prüfe Werkzeuge =="
for t in git python3 cmake gcc; do
  printf '  %-10s' "$t"; command -v "$t" >/dev/null && echo ok || echo "FEHLT"
done
printf '  %-10s' "ISO-Extraktor"
if command -v extract-xiso >/dev/null || command -v xdvdfs >/dev/null; then echo ok
else echo "FEHLT (extract-xiso oder xdvdfs installieren)"; fi

if [ "$(uname -s)" = "Linux" ]; then
  echo
  echo "Für den Linux-Build (SDL2/OpenGL/OpenSSL/ffmpeg) einmalig ausführen:"
  echo "  bash xboxrecomp/tools/linux/install_deps.sh     # braucht sudo"
fi
echo
echo "Fertig. Nächster Schritt: scripts/extract_iso.sh <SSX3-Xbox.iso>"
