#!/usr/bin/env bash
# A zero corner radius must keep the complete border square. Euclidean offset
# contours round an otherwise square outer corner, so sample its extreme pixel.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/square-border-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/square-border.png"
readonly TOTAL_WIDTH=9

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 1
outer_border_width = 8
corner_radius = 0
border_focused = "#00FF00"
border_unfocused = "#00FF00"
outer_border_color = "#200000"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" square-border 640 480 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "square border client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.5

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "square-border")')
if [[ -z $window ]]; then
  echo "square border client was not registered: $("$UMBRIEL" windows --json)"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")

grim -o HEADLESS-1 "$SCREENSHOT"
corner=$(magick "$SCREENSHOT" -alpha off \
  -crop "1x1+$((x - TOTAL_WIDTH))+$((y - TOTAL_WIDTH))" +repage \
  -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:)
read -r corner_red corner_green corner_blue <<< "$corner"
if (( corner_red < 30 || corner_green > 1 || corner_blue > 1 )); then
  echo "zero-radius border outer corner was rounded away: corner=$corner window=$window"
  exit 1
fi

echo "zero-radius border kept its square outer corner: corner=$corner"
