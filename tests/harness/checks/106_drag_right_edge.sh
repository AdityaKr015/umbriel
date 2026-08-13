#!/usr/bin/env bash
# The right edge of an overflowing scrolling workspace must remain a reachable
# drop target for the end of the strip, even while later columns are off-screen.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
readonly OVERVIEW_RIGHT=959
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
CLIENT_PIDS=()

cleanup() {
  "$UMBRIEL" msg overview-close > /dev/null 2>&1 || true
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
    foot --title="right-edge-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
}

for id in $(seq 1 6); do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
sleep 0.5

# Return focus and scroll to the first column, leaving most of the strip beyond
# the right edge. This is the state where layout-space nearest-gap selection
# cannot reach the strip's final gap.
for _ in $(seq 1 5); do
  "$UMBRIEL" msg window-focus-left > /dev/null
done
sleep 0.5

windows=$("$UMBRIEL" windows --json)
source_title=$(jq -r '.[] | select(.focused) | .title' <<< "$windows")
if [[ $source_title != right-edge-1 ]]; then
  echo "expected the first column to be focused: $windows"
  exit 1
fi
start_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.focused) | ($origin + ((.x + .w / 2) * $zoom) | round)' <<< "$windows")
start_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.focused) | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
pointer move "$start_x" "$start_y" press "$BTN_LEFT" move "$OVERVIEW_RIGHT" 360 release "$BTN_LEFT"
sleep 0.2
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6

windows=$("$UMBRIEL" windows --json)
source_x=$(jq -r --arg title "$source_title" '.[] | select(.title == $title) | .x' <<< "$windows")
rightmost_other_x=$(jq -r --arg title "$source_title" '[.[] | select(.title != $title) | .x] | max' <<< "$windows")
if (( source_x <= rightmost_other_x )); then
  echo "right-edge drop did not append $source_title to the strip: $windows"
  exit 1
fi

echo "right-edge drop appended the dragged column after off-screen columns"
