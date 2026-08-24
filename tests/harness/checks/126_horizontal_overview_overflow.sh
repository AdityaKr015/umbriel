#!/usr/bin/env bash
# Horizontal overview cards may extend beyond the scaled workspace background while remaining inside the output.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.horizontal-overview-overflow.bak"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/horizontal-overview-overflow.png"
CLIENT_PIDS=()

cleanup() {
  "$UMBRIEL" msg overview-close > /dev/null 2>&1 || true
  if [[ -f $CONFIG_BACKUP ]]; then
    cp "$CONFIG_BACKUP" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
  fi
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

spawn_client() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1000
border_width = 0
outer_border_width = 0
corner_radius = 0
backdrop_color = "#000000FF"

[overview]
zoom = 0.5
background_tint = "#000000FF"
workspace_background = "#000000FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client horizontal-overflow-first
wait_for_count 1
spawn_client horizontal-overflow-second
wait_for_count 2
spawn_client horizontal-overflow-third
wait_for_count 3

first_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "horizontal-overflow-first") | .id')
if [[ -z $first_id ]]; then
  echo "could not resolve the first horizontal column: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
sleep 0.2
"$UMBRIEL" msg overview-open > /dev/null
sleep 1.1
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 grim "$SCREENSHOT"
blue=$(magick "$SCREENSHOT" -crop 20x20+1090+350 -format '%[fx:round(255*mean.b)]' info:)
if (( blue < 80 )); then
  echo "horizontal card was clipped to the workspace background: blue=$blue"
  exit 1
fi

echo "horizontal overview cards remain visible across the output"
