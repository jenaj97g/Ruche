#!/bin/bash
# Runs Ruche against the local live-radio simulator and checks title timing.
# Usage: tests/run_title_tests.sh <ruche binary or AppImage> [kinds...]
set -u
BIN=$(readlink -f "$1"); shift
KINDS=${*:-"icy extinf packed tsid3"}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$(mktemp -d)}
DURATION=${DURATION:-55}
JOIN_DELAY=${JOIN_DELAY:-10}
mkdir -p "$WORK"
python3 -c "import sys; sys.path.insert(0, '$HERE'); import stream_server as s; s.make_audio('$WORK', 300)"
port=8780
pids=()
for k in $KINDS; do
  port=$((port + 1))
  d="$WORK/$k"; mkdir -p "$d/home" "$d/run"; chmod 700 "$d/run"
  python3 "$HERE/stream_server.py" --port $port --dir "$WORK" --log "$d/server.jsonl" >"$d/server.out" 2>&1 &
  echo $! >"$d/server.pid"
  case $k in
    icy) url="http://127.0.0.1:$port/icy" ;;
    *) url="http://127.0.0.1:$port/$k/live.m3u8" ;;
  esac
  (
    sleep "$JOIN_DELAY"
    HOME="$d/home" XDG_RUNTIME_DIR="$d/run" RUCHE_DEBUG=${RUCHE_DEBUG:-} RUCHE_NO_HLS_RELAY=${RUCHE_NO_HLS_RELAY:-} timeout $((DURATION + 20)) \
      xvfb-run -a -s "-screen 0 1280x800x24" "$BIN" --mpv-opt ao=null --title-log "$d/titles.tsv" \
      --quit-after "$DURATION" --screenshot "$d/shot.png" --screenshot-delay 30000 "$url" >"$d/app.log" 2>&1
  ) &
  pids+=($!)
done
wait "${pids[@]}"
rc=0
for k in $KINDS; do
  d="$WORK/$k"
  kill "$(cat "$d/server.pid")" 2>/dev/null
  python3 "$HERE/check_titles.py" "$k" "$d/server.jsonl" "$d/titles.tsv" || rc=1
done
echo "work dir: $WORK"
exit $rc
