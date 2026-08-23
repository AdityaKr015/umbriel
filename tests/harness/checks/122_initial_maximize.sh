#!/usr/bin/env bash
# A saved client maximize request must not override Umbriel's opening layout policy.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-client.log"
CLIENT_PID=

cleanup() {
  if [[ -n $CLIENT_PID ]]; then
    kill -KILL "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 REQUEST_MAXIMIZED=1 \
  "$CLIENT" initial-maximize > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

for _ in $(seq 40); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "maximize client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

sleep 0.3
if grep -q '^configured-maximized$' "$CLIENT_LOG"; then
  echo "opening client maximize request was accepted"
  exit 1
fi

echo "opening client maximize request was ignored"
