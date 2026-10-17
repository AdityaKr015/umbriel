#!/usr/bin/env bash
# A tiled neighbour that redraws at its new size mid-reflow crossfades from its outgoing frame instead of switching in
# one frame, and the crossfade ends with the reflow even when the redraw lands halfway through it. The survivor paints
# red at its first size and green at every later one, so a blended pixel exists only while both frames are composed.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/tiled-resize-crossfade.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "master"

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = false

[animation.dim_unfocused]
enabled = false

[animation.windows_move]
enabled = true
duration_ms = 1000
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  local title=$1
  shift
  env "$@" "$UMBRIEL_UNMAP_CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

# The survivor keeps the left part of the output in both layouts, so this point shows it throughout the reflow.
survivor_rgb() {
  grim "$IMAGE"
  magick "$IMAGE" -alpha off -crop '8x8+146+356' +repage \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:
}

mkfifo "$UMBRIEL_RUNTIME_DIR/survivor.fifo"
env FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFF00FF00 RESIZE_ON_STDIN=1 "$UMBRIEL_UNMAP_CLIENT" \
  resize-crossfade-survivor 1200 700 < "$UMBRIEL_RUNTIME_DIR/survivor.fifo" \
  > "$UMBRIEL_RUNTIME_DIR/resize-crossfade-survivor.log" 2>&1 &
exec {survivor_fd}> "$UMBRIEL_RUNTIME_DIR/survivor.fifo"
await_lines "$UMBRIEL_RUNTIME_DIR/resize-crossfade-survivor.log" mapped 1
"$UMBRIEL" settle
survivor_w() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "resize-crossfade-survivor") | .w'
}
first_w=$(survivor_w)
read -r red green blue < <(survivor_rgb)
if ! ((red > 220 && green < 30 && blue < 30)); then
  echo "setup did not settle the survivor on its first red frame: $red $green $blue"
  exit 1
fi

# Animation time only moves by clock-advance; the reflow runs a 1000 ms linear timeline from the frozen instant.
"$UMBRIEL" clock-freeze
spawn resize-crossfade-opener FILL_COLOR=0xFF0000FF
for _ in $(seq 80); do
  [[ $(survivor_w) != "$first_w" ]] && break
  sleep 0.025
done
# The survivor redraws halfway through the reflow; the client reports once the compositor has handled that commit.
"$UMBRIEL" clock-advance 500
printf r >&"$survivor_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/resize-crossfade-survivor.log" resized 1
"$UMBRIEL" clock-advance 250
read -r mid_red mid_green mid_blue < <(survivor_rgb)
# 950 ms: the reflow has 5% left, so the fade it drives is nearly done. A fade on a clock of its own, restarted at the
# redraw, would still be about half red here.
"$UMBRIEL" clock-advance 200
read -r late_red late_green late_blue < <(survivor_rgb)
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" settle
read -r end_red end_green end_blue < <(survivor_rgb)

if ! ((mid_red >= 40 && mid_red <= 215 && mid_green >= 40 && mid_green <= 215 && mid_blue < 30)); then
  echo "survivor did not crossfade from its outgoing frame mid-reflow: $mid_red $mid_green $mid_blue"
  exit 1
fi
if ! ((late_red < 60 && late_green > 190 && late_blue < 30)); then
  echo "the crossfade outlasted the reflow: $late_red $late_green $late_blue at 950 ms"
  exit 1
fi
if ! ((end_red < 30 && end_green > 220 && end_blue < 30)); then
  echo "survivor did not finish on its redrawn frame: $end_red $end_green $end_blue"
  exit 1
fi

echo "the late redraw crossfaded red -> green ($mid_red $mid_green, then $late_red $late_green) with the reflow"
