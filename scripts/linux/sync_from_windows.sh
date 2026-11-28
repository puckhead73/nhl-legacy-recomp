#!/usr/bin/env bash
# WSL helper: mirror the Windows checkout (incl. the untracked generated/ codegen
# output) into the Linux filesystem, where builds are fast. Build outputs and the
# Windows-side SDK checkout are skipped (Linux uses its own, see setup_sdk.sh).
#
# Usage (inside WSL): scripts/linux/sync_from_windows.sh <windows_repo> [dest]
#   e.g. bash /mnt/c/Users/me/.../nhl-legacy-recomp/scripts/linux/sync_from_windows.sh \
#          /mnt/c/Users/me/.../nhl-legacy-recomp ~/nhl-legacy-recomp
set -euo pipefail
src=${1:?windows repo path (/mnt/c/...)}
dst=${2:-$HOME/nhl-legacy-recomp}
mkdir -p "$dst"
rsync -a --delete \
  --exclude '/out/' --exclude '/third_party/rexglue-sdk/' \
  --exclude '/.git/' --exclude '/assets/' \
  "$src"/ "$dst"/
# Windows checkouts may carry CRLF shell scripts; bash needs LF.
find "$dst/scripts/linux" -name '*.sh' -exec sed -i 's/\r$//' {} + -exec chmod +x {} +
echo "synced $src -> $dst"
