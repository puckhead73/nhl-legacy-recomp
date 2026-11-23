#!/usr/bin/env bash
# Install everything needed to build the ReXGlue SDK + NHL Legacy natively for
# Linux on Ubuntu 24.04 (noble) - e.g. WSL2. Mirrors the SDK's own CI
# (third_party/rexglue-sdk/.github/workflows/_build-platform.yaml): clang/lld 20
# from apt.llvm.org, Vulkan headers/loader + shader tools from the Ubuntu archive,
# and the X11/Wayland/audio/input dev packages SDL3 needs to enable its drivers
# (without them SDL3 builds, but with no video/audio backend at runtime).
#
# Binaries built on noble (glibc 2.39) also run on SteamOS 3.x (glibc 2.41).
#
# Usage: bash scripts/linux/install_build_deps.sh   (asks for your sudo password)
set -euo pipefail

sudo apt-get update
sudo apt-get install -y wget gnupg lsb-release software-properties-common

# clang 20 + lld 20 (Ubuntu 24.04 ships clang 18; the SDK needs 20+).
if ! command -v clang-20 >/dev/null; then
  tmp=$(mktemp -d)
  wget -qO "$tmp/llvm.sh" https://apt.llvm.org/llvm.sh
  sudo bash "$tmp/llvm.sh" 20
  rm -rf "$tmp"
fi
sudo apt-get install -y clang-20 lld-20 llvm-20
sudo update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-20   200
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 200
sudo update-alternatives --install /usr/bin/ld.lld  ld.lld  /usr/bin/ld.lld-20  200

sudo apt-get install -y \
  cmake ninja-build build-essential pkg-config git curl unzip autoconf python3 \
  libvulkan-dev glslc glslang-tools spirv-tools \
  libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev \
  libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
  libudev-dev libdbus-1-dev libgtk-3-dev

echo
clang --version | head -1
cmake --version | head -1
echo "[deps] done"
