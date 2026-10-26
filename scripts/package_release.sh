#!/usr/bin/env bash
# Builds the release packages into dist/ (no game files in them):
#   dist/SSX3-Recomp-<version>-linux-x86_64.tar.gz    built in an Ubuntu 24.04 container -> runs on glibc 2.39+
#   dist/SSX3-Recomp-<version>-windows-x86_64.zip     MinGW cross build (scripts/build_win.sh)
#   scripts/package_release.sh v0.1.0
# Needs: docker (or podman) for the Linux build, mingw-w64-gcc and zip on the host for the Windows build, and the generated code (scripts/run_pipeline.sh).
#   SKIP_LINUX=1 / SKIP_WINDOWS=1   build only one of them
source "$(dirname "$0")/_common.sh"
VERSION="${1:?Usage: scripts/package_release.sh <version, e.g. v0.1.0>}"
[ -f "$GEN_DIR/recomp_funcs.h" ] || { echo "No generated code. First run: scripts/run_pipeline.sh" >&2; exit 1; }
DIST="$ROOT/dist"
JOBS="${JOBS:-8}"
mkdir -p "$DIST"
apply_runtime_patches

# Files every package gets: the player README, licenses, game_files/ with a note
common_files() {  # common_files <package dir>
  local pkg="$1"
  mkdir -p "$pkg/game_files"
  sed "s/@VERSION@/$VERSION/" "$ROOT/scripts/release/README.txt" > "$pkg/README.txt"
  cp "$ROOT/scripts/release/game_files_README.txt" "$pkg/game_files/README.txt"
  cp "$ROOT/LICENSE" "$pkg/LICENSE.txt"
  mkdir -p "$pkg/licenses"
  cp "$TOOLKIT/LICENSE" "$pkg/licenses/xboxrecomp-MIT.txt"
  cp "$TOOLKIT/NOTICE" "$pkg/licenses/xboxrecomp-NOTICE.txt"
  cp "$TOOLKIT/LICENSES/LGPL-2.1.txt" "$pkg/licenses/LGPL-2.1.txt"
  cp "$ROOT/third_party/volk/LICENSE.md" "$pkg/licenses/volk-MIT.txt"
}

if [ "${SKIP_LINUX:-0}" != 1 ]; then
  # Ubuntu 24.04 container (docker/Dockerfile) -> glibc 2.39, runs on older distros and SteamOS too
  BUILD_DIR="$ROOT/build-release-linux" JOBS="$JOBS" "$ROOT/scripts/docker.sh" scripts/build.sh
  PKG="$DIST/SSX3-Recomp-$VERSION-linux-x86_64"
  rm -rf "$PKG"; mkdir -p "$PKG"
  cp "$ROOT/build-release-linux/ssx3_recomp" "$PKG/"
  cp "$ROOT/scripts/run.sh" "$ROOT/scripts/install_game.sh" "$PKG/"
  common_files "$PKG"
  tar -C "$DIST" -czf "$PKG.tar.gz" "$(basename "$PKG")"
  echo "Linux package: $PKG.tar.gz"
fi

if [ "${SKIP_WINDOWS:-0}" != 1 ]; then
  JOBS="$JOBS" "$ROOT/scripts/build_win.sh"
  PKG="$DIST/SSX3-Recomp-$VERSION-windows-x86_64"
  rm -rf "$PKG"; mkdir -p "$PKG"
  cp "$ROOT/build-win/ssx3_recomp.exe" "$ROOT/scripts/run.ps1" "$ROOT/scripts/release/run.bat" \
     "$ROOT/scripts/install_game.ps1" "$ROOT/scripts/release/install_game.bat" "$PKG/"
  common_files "$PKG"
  # Windows line endings for the text files people open in Notepad
  for f in "$PKG"/*.txt "$PKG"/licenses/*.txt "$PKG"/game_files/*.txt "$PKG"/*.bat; do sed -i 's/\r*$/\r/' "$f"; done
  (cd "$DIST" && rm -f "$(basename "$PKG").zip" && zip -qr "$(basename "$PKG").zip" "$(basename "$PKG")")
  echo "Windows package: $PKG.zip"
fi

(cd "$DIST" && sha256sum $(ls SSX3-Recomp-"$VERSION"-*.tar.gz SSX3-Recomp-"$VERSION"-*.zip 2>/dev/null) > "SHA256SUMS-$VERSION.txt")
cat "$DIST/SHA256SUMS-$VERSION.txt"
