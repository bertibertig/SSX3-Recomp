# Build environment for SSX3-Recomp: Ubuntu 24.04 (glibc 2.39), so the binary also runs on older distros and the
# Steam Deck. Used by scripts/docker.sh (pipeline + build) and scripts/package_release.sh (release packages).
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates python3 python3-venv \
      libsdl2-dev libepoxy-dev libssl-dev libgl-dev glslc \
      mingw-w64 zip \
    && rm -rf /var/lib/apt/lists/*
# The pipeline's Python packages; _common.sh uses this venv instead of the repo's .venv
COPY requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements.txt
ENV SSX3_VENV=/opt/venv
