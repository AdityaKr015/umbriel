#!/usr/bin/env bash
# Saved client maximization is ignored by default and honored only when configured.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"

spawn_maximized_client() {
  local title=$1 log=$2
  env REQUEST_MAXIMIZED=1 "$CLIENT" "$title" > "$log" 2>&1 &
  CLIENT_PID=$!

  for _ in $(seq 40); do
    grep -q '^mapped$' "$log" && return 0
    sleep 0.05
  done
  echo "maximize client never mapped: $(cat "$log")"
  return 1
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

readonly DEFAULT_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-default.log"
spawn_maximized_client initial-maximize-default "$DEFAULT_LOG" || exit 1
sleep 0.3
if grep -q '^configured-maximized$' "$DEFAULT_LOG"; then
  echo "opening client maximize request was accepted by default"
  exit 1
fi
# The honored phase reuses CLIENT_PID for its own client, so the default-config
# one is stopped while it can still be signalled.
stop_client

printf '\nhonor_restored_maximize = true\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

readonly HONORED_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-honored.log"
spawn_maximized_client initial-maximize-honored "$HONORED_LOG" || exit 1
sleep 0.3
if ! grep -q '^configured-maximized$' "$HONORED_LOG"; then
  echo "configured opening client maximize request was ignored"
  exit 1
fi

echo "opening client maximize request ignored by default and honored when configured"
