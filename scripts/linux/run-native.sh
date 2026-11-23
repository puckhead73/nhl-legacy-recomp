#!/bin/bash
# Launch the NATIVE Linux build of the NHL Legacy recomp (no Proton).
# Works from a desktop terminal, a Steam shortcut, or over SSH.
cd "$(dirname "$0")"
export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
# Over SSH there is no X cookie; borrow the desktop session's Xwayland one.
if [ -z "$XAUTHORITY" ]; then
  XAUTHORITY=$(ls -t "$XDG_RUNTIME_DIR"/xauth_* 2>/dev/null | head -1)
  export XAUTHORITY
fi
export NHL_VK_BACKEND=1
exec ./nhllegacy "$@"
