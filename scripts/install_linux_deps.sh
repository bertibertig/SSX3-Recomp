#!/usr/bin/env bash
# Installs the native Linux build dependencies used by SSX3-Recomp.
set -euo pipefail

OS_RELEASE="${OS_RELEASE:-/etc/os-release}"

detect_family() {
  local id id_like
  [ -r "$OS_RELEASE" ] || return 1
  id=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID:-}")
  id_like=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID_LIKE:-}")

  case " $id $id_like " in
    *" debian "*|*" ubuntu "*|*" linuxmint "*|*" pop "*|*" kali "*) echo debian ;;
    *" arch "*|*" archlinux "*|*" manjaro "*|*" cachyos "*) echo arch ;;
    *" fedora "*|*" rhel "*|*" centos "*|*" rocky "*|*" almalinux "*) echo fedora ;;
    *" suse "*|*" opensuse "*|*" opensuse-leap "*|*" opensuse-tumbleweed "*) echo suse ;;
    *" alpine "*) echo alpine ;;
    *) return 1 ;;
  esac
}

run_privileged() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    echo "This installer needs root privileges; run it as root or install sudo." >&2
    return 1
  fi
}

family=$(detect_family) || {
  echo "Could not identify a supported Linux distribution from $OS_RELEASE." >&2
  echo "Supported families: Debian/Ubuntu, Arch, Fedora/RHEL, openSUSE, Alpine." >&2
  echo "Install a C toolchain, CMake, Python 3 with venv/pip, SDL2, OpenGL headers," >&2
  echo "libepoxy, OpenSSL, FFmpeg development libraries and glslc (shaderc) manually." >&2
  exit 1
}

case "$family" in
  debian)
    PKGS=(git python3 python3-venv python3-pip cmake build-essential pkg-config
      libsdl2-dev libgl1-mesa-dev libepoxy-dev libssl-dev libavcodec-dev
      libavformat-dev libavutil-dev libswscale-dev glslc)
    echo "Detected Debian/Ubuntu. Installing ${#PKGS[@]} packages..."
    run_privileged apt-get update
    run_privileged apt-get install -y "${PKGS[@]}"
    ;;
  arch)
    # base-devel provides the C toolchain and make; shaderc provides glslc.
    PKGS=(git base-devel cmake python python-pip sdl2 mesa libepoxy openssl ffmpeg shaderc)
    echo "Detected Arch. Installing ${#PKGS[@]} packages..."
    run_privileged pacman -S --needed "${PKGS[@]}"
    ;;
  fedora)
    PKGS=(git cmake gcc make pkgconf-pkg-config python3 python3-pip SDL2-devel
      mesa-libGL-devel libepoxy-devel openssl-devel ffmpeg-devel glslc)
    echo "Detected Fedora/RHEL. Installing ${#PKGS[@]} packages..."
    echo "(ffmpeg-devel may require RPM Fusion to be enabled.)"
    run_privileged dnf install -y "${PKGS[@]}"
    ;;
  suse)
    PKGS=(git gcc make cmake pkg-config python3 python3-pip python3-virtualenv
      SDL2-devel Mesa-libGL-devel libepoxy-devel libopenssl-devel shaderc)
    echo "Detected openSUSE. Installing ${#PKGS[@]} packages..."
    run_privileged zypper --non-interactive install "${PKGS[@]}"
    ;;
  alpine)
    PKGS=(git build-base cmake pkgconf python3 py3-pip py3-virtualenv sdl2-dev
      mesa-dev libepoxy-dev openssl-dev ffmpeg-dev shaderc)
    echo "Detected Alpine. Installing ${#PKGS[@]} packages..."
    run_privileged apk add --no-cache "${PKGS[@]}"
    ;;
esac

echo
echo "Linux build dependencies installed."
