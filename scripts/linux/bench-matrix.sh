#!/bin/bash
# Unattended performance comparison: runs the game once per configuration with
# the scripted controller (NHL_AUTOPLAY), lets it play a match, and prints the
# gameplay frame rate for each. Run it from the install directory (next to the
# nhllegacy binary), or pass the directory as $BENCH_DIR.
#
# Usage: bench-matrix.sh <game_data_root> [seconds_per_run]
#   CONFIGS="name:ENV=VAL,ENV=VAL;name2:..." overrides the default matrix.
#   CAP=0 (default) lifts the 60 fps cap so differences above 60 show up.
set -u
game=${1:?game data root}
secs=${2:-300}
dir=${BENCH_DIR:-$(pwd)}
cap=${CAP:-0}
configs=${CONFIGS:-"baseline:;single-pass:NHL_SINGLE_PASS=1;host-rt:NHL_VK_RT_PATH=fbo;host-rt+single-pass:NHL_VK_RT_PATH=fbo,NHL_SINGLE_PASS=1"}

cd "$dir" || exit 1
[ -x ./nhllegacy ] || { echo "no ./nhllegacy in $dir" >&2; exit 1; }
export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ -z "${XAUTHORITY:-}" ]; then
  XAUTHORITY=$(ls -t "$XDG_RUNTIME_DIR"/xauth_* 2>/dev/null | head -1); export XAUTHORITY
fi

printf '%-24s %8s %8s %8s %8s %10s\n' config match_s median p10 min draws
IFS=';' read -ra list <<< "$configs"
for entry in "${list[@]}"; do
  name=${entry%%:*}; envs=${entry#*:}
  args=(NHL_VK_BACKEND=1 NHL_AUTOPLAY=1 NHL_FPS_CAP="$cap")
  if [ -n "$envs" ]; then IFS=',' read -ra extra <<< "$envs"; args+=("${extra[@]}"); fi
  # The Linux build can stall during boot (intermittent, cause unknown); a run
  # that never reaches a match is retried up to RETRIES times (default 2).
  row=""
  for attempt in $(seq 0 "${RETRIES:-2}"); do
    before=$(ls logs/*.log 2>/dev/null | wc -l)
    env "${args[@]}" timeout -s KILL "$secs" ./nhllegacy --game_data_root "$game" >/dev/null 2>&1
    log=$(ls -t logs/*.log 2>/dev/null | head -1)
    if [ -z "$log" ] || [ "$(ls logs/*.log | wc -l)" -le "$before" ]; then
      row=$(printf '%-24s %s' "$name" "no log written"); break
    fi
    # Gameplay seconds only: the 1 Hz perf line with a match-sized draw count.
    row=$(grep 'nhl-vk-fps' "$log" \
      | sed 's/.*fps=\([0-9.]*\).*draws\/frame=\([0-9]*\).*/\1 \2/' \
      | awk '$2 >= 1000 && $2 <= 2600 { print $1, $2 }' | sort -n \
      | awk -v n="$name" -v t="$attempt" '{ f[NR] = $1; d += $2 }
          END { if (!NR) { printf "%-24s %8d   (never reached a match, %d retries)", n, 0, t; exit }
                printf "%-24s %8d %8.1f %8.1f %8.1f %10.0f", n, NR, f[int(NR/2)+1], f[int(NR*0.1)+1], f[1], d/NR
                if (t) printf "   (after %d retries)", t }')
    case "$row" in *"never reached"*) sleep 3 ;; *) break ;; esac
  done
  echo "$row"
  sleep 3
done
