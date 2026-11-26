#!/usr/bin/env bash
# Fetch + pin + patch the ReXGlue SDK (Linux / macOS counterpart of setup_sdk.ps1).
#
# Same pin and patch set as setup_sdk.ps1: the title patch
# (sdk/rexglue-vulkan-nhl-legacy-bd9b519.patch) and the bcctr codegen fix.
# The vendored high-cut deps (plume / XenosRecomp, tools/fetch_thirdparty.ps1) are
# NOT fetched: they are only used by the Windows-only NHLLEGACY_D3D12 build.
#
# Usage: scripts/setup_sdk.sh [sdk_dir]   (default $REXGLUE_SDK_SRC, else ../rexglue-sdk)
#
# The default is a SIBLING of the repo, not third_party/: `rexglue codegen` runs a
# migration scan over the whole project tree, and with the SDK inside it the scan
# proposes rewriting the SDK's own sources (src/rexglue/commands/migration_scan.cpp
# holds the legacy identifiers as string literals). Never accept that prompt for SDK
# paths. Pass third_party/rexglue-sdk explicitly to match setup_sdk.ps1 anyway.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${1:-${REXGLUE_SDK_SRC:-$(dirname "$repo")/rexglue-sdk}}"
pin="${REXGLUE_PIN:-bd9b51918faa71f962fa38af6d5f744af9aba2ce}"

if [ ! -d "$sdk/.git" ]; then
  echo "[sdk] cloning rexglue-sdk -> $sdk"
  git clone --filter=blob:none https://github.com/rexglue/rexglue-sdk "$sdk"
fi
echo "[sdk] pinning $pin"
git -C "$sdk" fetch -q origin "$pin" 2>/dev/null || true
git -C "$sdk" checkout -q "$pin"
# Shallow submodules: FFmpeg alone is ~1 GB with full history.
git -C "$sdk" submodule update --init --recursive --depth 1 --jobs 8

for p in rexglue-vulkan-nhl-legacy-bd9b519.patch rexglue-codegen-bcctr-tail-fix.patch; do
  if git -C "$sdk" apply --check "$repo/sdk/$p" 2>/dev/null; then
    git -C "$sdk" apply "$repo/sdk/$p"
    echo "[sdk] $p applied"
  elif git -C "$sdk" apply --check --reverse "$repo/sdk/$p" 2>/dev/null; then
    echo "[sdk] $p already applied - OK"
  else
    echo "[sdk] $p does not apply (tree modified?). Reset with:" >&2
    echo "      git -C \"$sdk\" checkout -- . && git -C \"$sdk\" submodule foreach git checkout -- ." >&2
    exit 1
  fi
done

echo
echo "SDK ready at $sdk (commit $pin + title patch)."
echo "Next: REXGLUE_SDK_SRC=\"$sdk\" scripts/build_linux.sh   (see docs/switch-port/linux-arm64-build.md)"
