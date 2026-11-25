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
# Fastest measured configuration on a Steam Deck: host render targets +
# single-pass scene. Set either variable yourself to override.
export NHL_VK_RT_PATH="${NHL_VK_RT_PATH:-fbo}"
export NHL_SINGLE_PASS="${NHL_SINGLE_PASS:-1}"
# Under Steam, use Steam Input's virtual gamepad instead of opening the Deck's
# controller hardware directly (the two fight over it and input drops out).
if [ -n "$SteamEnv" ]; then export SDL_JOYSTICK_HIDAPI_STEAMDECK="${SDL_JOYSTICK_HIDAPI_STEAMDECK:-0}"; fi
exec ./nhllegacy "$@"
