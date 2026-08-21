#!/usr/bin/env bash
# Dragging a single-view scrolling column must retain its width when reordered. The overview is used because virtual pointer events can drive its unmodified drag
# path without synthesizing a compositor modifier key.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.drag-width.bak"
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

pointer() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^wide-drag$"
default_width = 0.75
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client terminal-drag
wait_for_count 1
spawn_client wide-drag
wait_for_count 2
sleep 0.5

windows=$("$UMBRIEL" windows --json)
wide_before=$(jq -r '.[] | select(.title == "wide-drag") | .w' <<< "$windows")
if [[ $wide_before != 942 ]]; then
  echo "window rule did not produce the expected 0.75 width: $windows"
  exit 1
fi

start_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "wide-drag") | ($origin + ((.x + .w / 2) * $zoom) | round)' <<< "$windows")
start_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "wide-drag") | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
# The left side of the overview row maps to the gap before the terminal.
pointer move "$start_x" "$start_y" press "$BTN_LEFT" move 345 360 release "$BTN_LEFT"
sleep 0.2
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6

windows=$("$UMBRIEL" windows --json)
wide_after=$(jq -r '.[] | select(.title == "wide-drag") | .w' <<< "$windows")
wide_x=$(jq -r '.[] | select(.title == "wide-drag") | .x' <<< "$windows")
terminal_x=$(jq -r '.[] | select(.title == "terminal-drag") | .x' <<< "$windows")
if [[ $wide_after != "$wide_before" ]]; then
  echo "drag changed the wide column from $wide_before to $wide_after pixels: $windows"
  exit 1
fi
if (( wide_x >= terminal_x )); then
  echo "drag did not move the wide column left of the terminal: $windows"
  exit 1
fi

echo "drag reorder retained the 0.75 scrolling column width"
