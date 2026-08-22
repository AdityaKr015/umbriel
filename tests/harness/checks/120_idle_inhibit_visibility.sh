#!/usr/bin/env bash
# A visible toplevel inhibits idle, then stops inhibiting as soon as its
# workspace becomes inactive while its process and protocol objects stay alive.
set -euo pipefail

readonly CLIENT="${UMBRIEL_IDLE_INHIBIT_CLIENT:-./build-debug/idle-inhibit-client}"
CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/idle-inhibit-client.log"
CLIENT_PID=

cleanup() {
  if [[ -n $CLIENT_PID ]]; then
    kill -KILL "$CLIENT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  "$CLIENT" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

for _ in $(seq 40); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "idle inhibitor client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

sleep 0.35
if grep -q '^idled$' "$CLIENT_LOG"; then
  echo "visible surface failed to inhibit idle"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:2 > /dev/null
for _ in $(seq 40); do
  grep -q '^idled$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^idled$' "$CLIENT_LOG"; then
  echo "hidden surface continued to inhibit idle: $(cat "$CLIENT_LOG")"
  exit 1
fi

echo "idle inhibitor follows toplevel workspace visibility"
