#!/usr/bin/env bash
# Build the ReXGlue SDK + the NHL Legacy port on Linux (native or cross to ARM64).
#
#   scripts/build_linux.sh [arm64|amd64] [Release|RelWithDebInfo|Debug] [sdk|game|all]
#
# arm64 on an x86_64 host cross-compiles with cmake/toolchains/linux-aarch64-clang.cmake
# (and runs any aarch64 build-time tool under qemu-user); arm64 on an aarch64 host
# (e.g. Switch L4T) builds natively. Prereqs: docs/switch-port/linux-arm64-build.md.
#
# Run scripts/setup_sdk.sh first. The game step needs generated/ (codegen output:
# `cmake --build <game build dir> --target nhllegacy_codegen` with the XEX, or
# nhllegacy_codegen_emu when cross-compiling - runs the aarch64 rexglue under qemu);
# without it the host code compiles but the link fails on the guest functions.
set -euo pipefail

arch="${1:-arm64}"
cfg="${2:-Release}"
what="${3:-all}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Same default as setup_sdk.sh: a sibling of the repo (see the note there on why
# not third_party/). Falls back to third_party/rexglue-sdk if that is where it is.
sdk="${REXGLUE_SDK_SRC:-$(dirname "$repo")/rexglue-sdk}"
if [[ ! -d "$sdk" && -d "$repo/third_party/rexglue-sdk" ]]; then
  sdk="$repo/third_party/rexglue-sdk"
fi
host_arch="$(uname -m)"
jobs="${JOBS:-$(nproc)}"

common=(-G Ninja "-DCMAKE_BUILD_TYPE=$cfg" -Wno-dev)
case "$arch" in
  arm64|aarch64)
    arch=arm64
    if [[ "$host_arch" != aarch64 && "$host_arch" != arm64 ]]; then
      common+=("-DCMAKE_TOOLCHAIN_FILE=$repo/cmake/toolchains/linux-aarch64-clang.cmake")
    else
      common+=(-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
               -DCMAKE_C_FLAGS=-mcpu=cortex-a57 -DCMAKE_CXX_FLAGS=-mcpu=cortex-a57)
    fi ;;
  amd64|x86_64)
    arch=amd64
    common+=(-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
             -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3) ;;
  *) echo "unknown arch '$arch' (arm64|amd64)" >&2; exit 2 ;;
esac

sdk_build="$sdk/out/build/linux-$arch"
sdk_install="${REXGLUE_SDK_INSTALL:-$sdk/out/install/linux-$arch}"
game_build="$repo/out/build/linux-$arch-vk"

if [[ "$what" == sdk || "$what" == all ]]; then
  # Tracy off: matches the shipped Release runtime (no profiler instrumentation).
  cmake -S "$sdk" -B "$sdk_build" "${common[@]}" \
    "-DCMAKE_INSTALL_PREFIX=$sdk_install" \
    -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF
  cmake --build "$sdk_build" -j "$jobs"
  cmake --install "$sdk_build"
fi

if [[ "$what" == game || "$what" == all ]]; then
  cmake -S "$repo" -B "$game_build" "${common[@]}" \
    "-DCMAKE_PREFIX_PATH=$sdk_install" \
    -DNHLLEGACY_D3D12=OFF -DNHLLEGACY_VULKAN_BACKEND=ON
  cmake --build "$game_build" -j "$jobs"
  echo "Built: $game_build/nhllegacy"
fi
