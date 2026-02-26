#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[perf] building"
cmake --build build -j"$(nproc)" >/dev/null

echo "[perf] launching app for smoke window (12s)"
QT_QPA_PLATFORM=offscreen timeout 12s ./build/roscoppe >/tmp/roscoppe_perf.out 2>/tmp/roscoppe_perf.err || true

TELEM_FILE="$ROOT_DIR/logs/telemetry_last_exit.json"
if [[ ! -f "$TELEM_FILE" ]]; then
  TELEM_FILE="$ROOT_DIR/logs/telemetry_live.json"
fi
if [[ ! -f "$TELEM_FILE" ]]; then
  echo "[perf] telemetry file missing: $TELEM_FILE"
  echo "[perf] app may have been force-killed before clean shutdown"
  exit 1
fi

echo "[perf] telemetry summary"
jq '.requests_per_minute,
    .gauges["ui.event_loop_lag_ms"],
    .gauges["memory.rss_kb"],
    .gauges["queue.offline_remote_actions"],
    .durations["sync.duration_ms"],
    .durations["ui.render.process_list_ms"]' "$TELEM_FILE"

echo "[perf] smoke complete"
