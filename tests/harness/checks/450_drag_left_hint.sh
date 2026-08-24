#!/usr/bin/env bash
# An overflowing scrolling strip must show its prepend hint at the left edge.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
readonly OVERFLOW_LEFT=321
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

spawn_client() {
  foot --config=/dev/null --override=colors.background=000000 \
    --title="left-hint-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
insert_hint_color = "#FF0000FF"

[overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 6); do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
sleep 0.5

for _ in $(seq 1 5); do
  "$UMBRIEL" msg window-focus-left > /dev/null
done
sleep 0.5

windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.focused) | ($origin + ((.x + .w - 10) * $zoom) | round)' <<< "$windows")
start_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.focused) | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$start_x" "$start_y" press "$BTN_LEFT" move "$OVERFLOW_LEFT" 360 pause 1200 release "$BTN_LEFT" &
POINTER_PID=$!
sleep 0.5

screenshot="$UMBRIEL_RUNTIME_DIR/drag-left-hint.png"
grim "$screenshot"

red=$(magick "$screenshot" -crop 100x240+350+240 -colorspace RGB \
  -format '%[fx:round(255*mean.r)]' info:)
green=$(magick "$screenshot" -crop 100x240+350+240 -colorspace RGB \
  -format '%[fx:round(255*mean.g)]' info:)
if (( red < green + 35 )); then
  echo "left-edge prepend hint was not visible: red=$red green=$green"
  exit 1
fi

wait "$POINTER_PID"
echo "left-edge prepend hint was visible: red=$red green=$green"
