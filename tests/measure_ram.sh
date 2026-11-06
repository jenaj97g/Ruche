#!/bin/bash
# Starts Ruche under Xvfb, waits, then prints its memory use.
# Usage: tests/measure_ram.sh <label> <seconds> <ruche binary or AppImage> [ruche args...]
# Env: RUCHE_HOME (isolated $HOME to use), MEASURE_SAMPLES (default 3)
set -u
label=$1; secs=$2; bin=$(readlink -f "$3"); shift 3
home=${RUCHE_HOME:-$(mktemp -d)}
mkdir -p "$home/run"; chmod 700 "$home/run"
HOME="$home" XDG_RUNTIME_DIR="$home/run" xvfb-run -a -s "-screen 0 1440x900x24" "$bin" --mpv-opt ao=null "$@" >/dev/null 2>&1 &
runner=$!
pid=""
for _ in $(seq 1 100); do
  pid=$(pgrep -n -x ruche) && break
  sleep 0.1
done
[ -z "$pid" ] && { echo "$label: ruche did not start"; kill $runner; exit 1; }
sleep "$secs"
best=""
for _ in $(seq 1 ${MEASURE_SAMPLES:-3}); do
  best=$(awk '/^Rss:/{r=$2} /^Pss:/{p=$2} /^Private_Clean:/{pc=$2} /^Private_Dirty:/{pd=$2} /^Swap:/{s=$2}
              END{printf "%d %d %d", r, p, pc+pd}' /proc/$pid/smaps_rollup)
  sleep 1
done
read rss pss uss <<<"$best"
threads=$(ls /proc/$pid/task | wc -l)
libs=$(grep -c '\.so' /proc/$pid/maps | awk '{print $1}')
nlibs=$(awk '$6 ~ /\.so/ {print $6}' /proc/$pid/maps | sort -u | wc -l)
printf "%-34s RSS %6.1f MiB   PSS %6.1f MiB   private %6.1f MiB   threads %2d   shared libs %3d\n" \
  "$label" "$(echo "$rss/1024" | bc -l)" "$(echo "$pss/1024" | bc -l)" "$(echo "$uss/1024" | bc -l)" "$threads" "$nlibs"
helper=$(pgrep -f "^$bin" | head -1); [ -n "$helper" ] && [ "$helper" != "$pid" ] && awk -v l="$label" '/^Rss:/{printf "%-34s (image helper process: RSS %.1f MiB)\n", "", $2/1024}' /proc/$helper/smaps_rollup
kill "$pid" 2>/dev/null
wait $runner 2>/dev/null
