#!/usr/bin/env bash
# Height actions size the only window of a scrolling column, matching what dragging its bottom edge already does. The
# window keeps its top edge so the freed space below it is where a second window in the column goes.
set -euo pipefail

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

wait_for_windows() {
  local want=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $want window(s), got $count"
  return 1
}

# Output is 1280x720 with the shipped defaults: edgePad = 10, so a single row's stacking extent is 700. Two rows share
# 700 - totalGap = 688.
readonly FULL_H=700
readonly EDGE_PAD=10
readonly TOTAL_GAP=12

wait_for_geometry() {
  local title=$1 want_h=$2 want_y=$3 message=$4 windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --arg title "$title" --argjson h "$want_h" --argjson y "$want_y" '
      [.[] | select(.title == $title)] as $w
      | ($w | length == 1) and ($w[0].h - $h | fabs <= 2) and ($w[0].y - $y | fabs <= 2)
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "$message, got: $(jq -c '[.[] | {title, h, y}]' <<< "$windows")"
  return 1
}

foot --title=harness-solo sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_windows 1
wait_for_geometry harness-solo "$FULL_H" "$EDGE_PAD" "expected the lone window to open at full column height"

# Presets are [0.333, 0.5, 0.667]; cycling forward from full height wraps to the smallest. The top edge stays put, so
# only the height changes.
accepts window-cycle-height
wait_for_geometry harness-solo 233 "$EDGE_PAD" "window-cycle-height did not shrink the lone window to a third of the column"

accepts window-cycle-height
wait_for_geometry harness-solo 350 "$EDGE_PAD" "window-cycle-height did not step the lone window to half the column"

# The explicit setter agrees with the cycle, and full height reclaims the freed space.
accepts window-set-height:0.667
wait_for_geometry harness-solo 467 "$EDGE_PAD" "window-set-height did not size the lone window to two thirds of the column"

accepts window-set-height:1.0
wait_for_geometry harness-solo "$FULL_H" "$EDGE_PAD" "window-set-height:1.0 did not restore the lone window to full column height"

# The freed space is what a second window in the column takes: shrink to a third, then stack a client below it.
accepts window-set-height:0.333
wait_for_geometry harness-solo 233 "$EDGE_PAD" "window-set-height:0.333 did not shrink the lone window from its bottom edge"

foot --title=harness-below sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_windows 2
accepts window-consume-or-expel-left
wait_for_geometry harness-below $((FULL_H - TOTAL_GAP - 233)) $((EDGE_PAD + 233 + TOTAL_GAP)) \
  "the second window did not take the space the shrunk lone window freed"
wait_for_geometry harness-solo 233 "$EDGE_PAD" "stacking a second window moved or resized the shrunk first window"

echo "height actions sized the lone window from its bottom edge and the next window took the freed space"
