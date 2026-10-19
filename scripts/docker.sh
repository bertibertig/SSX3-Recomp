#!/usr/bin/env bash
# Runs a command in the Ubuntu 24.04 build container (docker/Dockerfile), with the repo mounted at the same path
# and files written as your own user. No build tools needed on the host besides docker or podman.
#   scripts/docker.sh scripts/run_pipeline.sh     # recompile game_files/default.xbe -> src/recomp/gen/
#   scripts/docker.sh scripts/build.sh            # -> build-docker/ssx3_recomp (BUILD_DIR, default for the container)
#   scripts/docker.sh scripts/build_win.sh        # Windows cross build -> build-docker-win/ssx3_recomp.exe
#   scripts/docker.sh bash                        # a shell in the container
# SSX3_ENGINE=podman picks the engine (default: docker, else podman); JOBS is passed through.
source "$(dirname "$0")/_common.sh"
[ $# -gt 0 ] || { echo "Usage: scripts/docker.sh <command> [args...]" >&2; exit 1; }
ENGINE="${SSX3_ENGINE:-$(command -v docker || command -v podman || true)}"
[ -n "$ENGINE" ] || { echo "docker or podman needed" >&2; exit 1; }
IMAGE=ssx3-recomp-build:ubuntu24.04
"$ENGINE" build -q -t "$IMAGE" -f "$ROOT/docker/Dockerfile" "$ROOT" >/dev/null
args=(--rm -v "$ROOT:$ROOT" -w "$ROOT" -e HOME=/tmp -e "BUILD_DIR=${BUILD_DIR:-$ROOT/build-docker}"
      -e "WIN_BUILD_DIR=${WIN_BUILD_DIR:-$ROOT/build-docker-win}")
[ -n "${JOBS:-}" ] && args+=(-e "JOBS=$JOBS")
[ -t 0 ] && [ -t 1 ] && args+=(-it)
if [[ "$(basename "$ENGINE")" == podman* ]]; then
  args+=(--userns=keep-id)                        # rootless podman: map your uid into the container
else
  args+=(--user "$(id -u):$(id -g)")
fi
exec "$ENGINE" run "${args[@]}" "$IMAGE" "$@"
