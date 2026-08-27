#!/usr/bin/env bash
# Border and surface shaders must use the same rounded-corner bounds. The client
# paints a full-window blue subsurface so a protruding corner is directly visible.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/subsurface-border-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/subsurface-border.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 1
outer_border_width = 8
corner_radius = 12
border_focused = "#00FF00"
border_unfocused = "#00FF00"
outer_border_color = "#200000"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" subsurface-border 640 480 animate > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "subsurface border client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.5

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "subsurface-border")')
if [[ -z $window ]]; then
  echo "subsurface border client was not registered: $("$UMBRIEL" windows --json)"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")

grim -o HEADLESS-1 "$SCREENSHOT"
sample() {
  magick "$SCREENSHOT" -alpha off -crop "1x1+$1+$2" +repage \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

# The tangent's first content pixel stays blue, proving the border does not
# thicken where the straight edge becomes a curve. The diagonal still carries
# focus-ring coverage, while the outer band and shared color seam stay opaque.
inside=$(sample "$((x + 7))" "$((y + 1))")
diagonal=$(sample "$((x + 3))" "$((y + 3))")
outer=$(sample "$x" "$y")
seam=$(sample "$((x + 5))" "$y")
read -r inside_red inside_green inside_blue <<< "$inside"
read -r diagonal_red diagonal_green diagonal_blue <<< "$diagonal"
read -r outer_red outer_green outer_blue <<< "$outer"
read -r seam_red seam_green seam_blue <<< "$seam"
if (( inside_green > 10 || inside_blue < 240 ||
      diagonal_green < 80 || diagonal_blue > 120 ||
      outer_red < 30 || outer_green > 1 || outer_blue > 1 ||
      seam_red < 10 || seam_green < 40 || seam_blue > 1 )); then
  echo "rounded border tangent was uneven: inside=$inside diagonal=$diagonal outer=$outer seam=$seam window=$window"
  exit 1
fi

echo "single-pass border kept a continuous tangent: inside=$inside diagonal=$diagonal outer=$outer seam=$seam"
