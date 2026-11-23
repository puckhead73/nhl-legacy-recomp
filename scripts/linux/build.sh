#!/usr/bin/env bash
# Native Linux build of the NHL Legacy port (SDK + game) - optimized, Vulkan.
#
# Usage: scripts/linux/build.sh [sdk|game|stage|all]   (default: all)
#   sdk   - configure + build + install the ReXGlue SDK (Release, Vulkan, no FFX)
#   game  - configure + build nhllegacy against that SDK
#   stage - assemble a runnable dir: nhllegacy + librexruntime.so ($ORIGIN rpath)
#
# Run from the repo root on a Linux filesystem (WSL: ~/..., not /mnt/c - see
# scripts/linux/sync_from_windows.sh). Needs scripts/linux/install_build_deps.sh
# and scripts/linux/setup_sdk.sh first. generated/ (codegen output) must exist:
# it is platform-neutral, so the Windows-side codegen output is reused as-is.
#
# Flags match the Windows optimized build (scripts/_build_vk_opt.bat): -O3,
# ThinLTO + lld, -march=x86-64-v3 (portable AVX2 floor - the Steam Deck's Zen 2
# supports it; never -march=native).
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${REXGLUE_SDK_SRC:-$HOME/rexglue-sdk}
sdk_build=$sdk/out/build/linux-amd64-nhl
sdk_install=$sdk/out/install/linux-amd64-nhl
game_build=$repo/out/build/linux-amd64-opt
stage=$repo/out/stage/linux-amd64
# Each recompiled game TU needs ~1 GB at -O3 (measured: 0.8-1.1 GB). Size the
# parallelism from memory that is AVAILABLE now, not installed: under WSL the
# VM's RAM limit is only a ceiling - when Windows itself is short on memory the
# VM thrashes and the OOM killer fires. One job per 2 GB available leaves
# headroom for the linker. Override with JOBS=n.
mem_gb=$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo)
mem_jobs=$(( mem_gb / 2 > 0 ? mem_gb / 2 : 1 ))
jobs=${JOBS:-$(( mem_jobs < $(nproc) ? mem_jobs : $(nproc) ))}
echo "[build] using $jobs parallel jobs (${mem_gb} GB available)"
what=${1:-all}

export CC=clang CXX=clang++
# -march goes in the BASE flags, like the upstream linux preset: vendored deps
# (snappy) compile AVX2 paths that need it outside the per-config flags.
base="-march=x86-64-v3 -flto=thin"
opt="-O3 -DNDEBUG"

build_sdk() {
  echo "== SDK: configure ($sdk_build)"
  cmake -S "$sdk" -B "$sdk_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_STANDARD=23 \
    "-DCMAKE_C_FLAGS=$base" "-DCMAKE_CXX_FLAGS=$base" \
    "-DCMAKE_C_FLAGS_RELEASE=$opt" "-DCMAKE_CXX_FLAGS_RELEASE=$opt" \
    "-DCMAKE_SHARED_LINKER_FLAGS=-flto=thin -fuse-ld=lld" \
    "-DCMAKE_EXE_LINKER_FLAGS=-flto=thin -fuse-ld=lld" \
    "-DCMAKE_INSTALL_PREFIX=$sdk_install" \
    -DREXGLUE_USE_VULKAN=ON \
    -DREXGLUE_ENABLE_FIDELITYFX=OFF \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
    -DREXGLUE_BUILD_TESTS=OFF \
    -DSNAPPY_HAVE_SSSE3=1 -DSNAPPY_HAVE_BMI2=1 -DSNAPPY_HAVE_X86_CRC32=1
  # ^ The SDK's vendored-deps setup replaces CMAKE_CXX_FLAGS while snappy runs
  #   its feature checks, so they run without -march and all fail - yet the real
  #   compile has -march=x86-64-v3 (__AVX__ set) and takes snappy's AVX path
  #   without <immintrin.h>. x86-64-v3 guarantees SSSE3/BMI2/SSE4.2, so preset
  #   the results instead of letting the checks misfire.
  echo "== SDK: build + install"
  cmake --build "$sdk_build" --parallel "$jobs"
  cmake --install "$sdk_build"
}

build_game() {
  [ -f "$repo/generated/rexglue.cmake" ] || {
    echo "generated/ is missing - sync it from the Windows checkout (codegen output)" >&2
    exit 1
  }
  echo "== game: configure ($game_build)"
  cmake -S "$repo" -B "$game_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    "-DCMAKE_C_FLAGS=$base" "-DCMAKE_CXX_FLAGS=$base" \
    "-DCMAKE_C_FLAGS_RELEASE=$opt" "-DCMAKE_CXX_FLAGS_RELEASE=$opt" \
    "-DCMAKE_EXE_LINKER_FLAGS=-flto=thin -fuse-ld=lld -Wl,--thinlto-jobs=$jobs" \
    "-DCMAKE_PREFIX_PATH=$sdk_install" \
    "-DREXGLUE_SDK_SRC=$sdk" \
    "-DCMAKE_BUILD_RPATH=\$ORIGIN" \
    -DNHLLEGACY_VULKAN_BACKEND=ON \
    -DNHLLEGACY_BUILD_PACKAGER=OFF -DNHLLEGACY_BUILD_TRACE_TOOLS=OFF
  echo "== game: build"
  cmake --build "$game_build" --target nhllegacy --parallel "$jobs"
}

stage_out() {
  echo "== stage -> $stage"
  rm -rf "$stage"
  mkdir -p "$stage"
  cp "$game_build/nhllegacy" "$stage/"
  cp -P "$sdk_install"/lib/librexruntime.so* "$stage/"
  # Make sure the binary finds the runtime beside itself.
  readelf -d "$stage/nhllegacy" | grep -E 'RUNPATH|RPATH' || true
  ls -la "$stage"
}

case "$what" in
  sdk) build_sdk ;;
  game) build_game ;;
  stage) stage_out ;;
  all) build_sdk; build_game; stage_out ;;
  *) echo "usage: $0 [sdk|game|stage|all]" >&2; exit 2 ;;
esac
