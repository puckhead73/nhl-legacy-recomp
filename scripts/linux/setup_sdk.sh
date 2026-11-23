#!/usr/bin/env bash
# Fetch + pin + patch the ReXGlue SDK for a native Linux build - the Linux
# counterpart of scripts/setup_sdk.ps1. Idempotent: re-run to repair.
#
# Usage: scripts/linux/setup_sdk.sh [sdk_dir]
#   sdk_dir defaults to $REXGLUE_SDK_SRC or ~/rexglue-sdk. Keep it on the Linux
#   filesystem (not /mnt/c) - WSL builds from Windows drives are very slow.
#
# Unlike the Windows script this does NOT fetch plume / XenosRecomp: they only
# serve the Windows-only D3D12 "high-cut" experiment, which Linux builds skip.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${1:-${REXGLUE_SDK_SRC:-$HOME/rexglue-sdk}}
pin=bd9b51918faa71f962fa38af6d5f744af9aba2ce
patches=(
  "$repo/sdk/rexglue-vulkan-nhl-legacy-bd9b519.patch"
  "$repo/sdk/rexglue-codegen-bcctr-tail-fix.patch"
  # POSIX timed multi-waits busy-spun (ms truncation -> sleep_for(0)).
  "$repo/sdk/rexglue-linux-wait-spin-fix.patch"
)

if [ ! -d "$sdk/.git" ]; then
  echo "[sdk] cloning rexglue-sdk -> $sdk"
  git clone https://github.com/rexglue/rexglue-sdk "$sdk"
fi
echo "[sdk] pinning $pin"
# The pin is not on upstream's default branch any more; fetch it explicitly.
git -C "$sdk" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$sdk" fetch origin "$pin"
git -C "$sdk" checkout -q "$pin"
git -C "$sdk" submodule update --init --recursive

for p in "${patches[@]}"; do
  name=$(basename "$p")
  # The repo may hand us CRLF patches (Windows checkout); the SDK tree is LF.
  tmp=$(mktemp)
  sed 's/\r$//' "$p" > "$tmp"
  if git -C "$sdk" apply --check "$tmp" 2>/dev/null; then
    git -C "$sdk" apply "$tmp"
    echo "[sdk] $name applied"
  elif git -C "$sdk" apply --check --reverse "$tmp" 2>/dev/null; then
    echo "[sdk] $name already applied - OK"
  else
    rm -f "$tmp"
    echo "[sdk] $name does not apply (tree modified?). Reset with:" >&2
    echo "      git -C \"$sdk\" checkout -- . && git -C \"$sdk\" submodule foreach --recursive git checkout -- ." >&2
    exit 1
  fi
  rm -f "$tmp"
done

echo "SDK ready at $sdk (commit $pin + title patch)."
echo "Next: scripts/linux/build.sh"
