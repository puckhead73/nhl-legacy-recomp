#!/bin/bash
# PGO profiling session for the native Linux build (see scripts/linux/build.sh
# pgo-gen). Runs the INSTRUMENTED binary (nhllegacy-pgogen, beside the normal
# one), then after NHL_PGO_DUMP_AFTER seconds (default 900 = 15 min) writes the
# profile to pgo/nhl-<pid>.profraw and exits by itself - no need to quit (Steam's
# "Exit Game" kills the process before the profile is written).
#
# Play like you normally would: menus, then a full match (faceoffs, play, replays).
# The instrumented build runs slower than normal - that's expected.
cd "$(dirname "$0")"
export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ -z "$XAUTHORITY" ]; then
  XAUTHORITY=$(ls -t "$XDG_RUNTIME_DIR"/xauth_* 2>/dev/null | head -1)
  export XAUTHORITY
fi
export NHL_VK_BACKEND=1
export NHL_PGO_DUMP_AFTER="${NHL_PGO_DUMP_AFTER:-900}"
mkdir -p pgo
export LLVM_PROFILE_FILE="$PWD/pgo/nhl-%p.profraw"
exec ./nhllegacy-pgogen "$@"
