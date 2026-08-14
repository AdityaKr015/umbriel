#!/usr/bin/env bash
# Opaque cards fade during an overview drag. Client transparency composes with
# the drag multiplier instead of bypassing the compositor-owned opacity.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.drag-opacity.bak"
CLIENT_PID=
POINTER_PID=

cleanup() {
  if [[ -n $POINTER_PID ]] && kill -0 "$POINTER_PID" 2>/dev/null; then
    kill -KILL "$POINTER_PID" 2>/dev/null || true
  fi
  "$UMBRIEL" msg overview-close > /dev/null 2>&1 || true
  if [[ -f $CONFIG_BACKUP ]]; then
    cp "$CONFIG_BACKUP" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
  fi
  if [[ -n $CLIENT_PID ]]; then
    kill -KILL "$CLIENT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

pointer() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

measure_drag_green() {
  local alpha=$1 title=$2
  local screenshot="$UMBRIEL_RUNTIME_DIR/$title.png"
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --config=/dev/null --override=colors.background=000000 --override="colors.alpha=$alpha" \
      --title="$title" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PID=$!
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
    sleep 0.25
  done
  if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
    echo "timed out waiting for $title" >&2
    return 1
  fi

  "$UMBRIEL" msg overview-open > /dev/null
  sleep 0.6
  # The single card is centered at (640, 360). Move it right while holding the
  # button, then keep the connection alive so the compositor retains the grab.
  pointer move 640 360 press "$BTN_LEFT" move 740 360 pause 1200 release "$BTN_LEFT" &
  POINTER_PID=$!
  sleep 0.5
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    grim "$screenshot"

  local green
  green=$(magick "$screenshot" -crop 40x40+680+430 -colorspace RGB \
    -format '%[fx:round(255*mean.g)]' info:)
  wait "$POINTER_PID"
  POINTER_PID=
  "$UMBRIEL" msg overview-close > /dev/null
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
  CLIENT_PID=
  for _ in $(seq 40); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq 0 ]] && break
    sleep 0.1
  done
  printf '%s\n' "$green"
}

cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
insert_hint_color = "#FF0000FF"

[overview]
background_tint = "#000000FF"
workspace_background = "#00FF00FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

opaque_green=$(measure_drag_green 1 drag-opaque)
if (( opaque_green < 40 || opaque_green > 210 )); then
  echo "opaque dragged card did not become semi-transparent: mean green=$opaque_green"
  exit 1
fi

transparent_green=$(measure_drag_green 0.5 drag-transparent)
if (( transparent_green <= opaque_green + 25 || transparent_green > 220 )); then
  echo "client alpha did not compose with drag opacity: opaque=$opaque_green transparent=$transparent_green"
  exit 1
fi

echo "drag opacity composed with client alpha: opaque=$opaque_green transparent=$transparent_green"
