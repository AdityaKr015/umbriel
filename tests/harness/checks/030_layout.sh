#!/usr/bin/env bash
# Real clients map into the scrolling layout and land on the geometry the layout math predicts. This is the regression net for the layout sizing path. The size a
# view is first configured with and the size the layout arranges it to must agree, or windows visibly resize on first paint.
set -euo pipefail

CLIENT_PIDS=()
cleanup() {
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

spawn_client() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --title="harness-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timed out waiting for $want window(s), saw: $("$UMBRIEL" windows --json)"
  return 1
}

# Output is 1280x720 (WLR_HEADLESS_OUTPUTS default mode). With the shipped defaults (gap 8, border 2, no outer border) the derived layout metrics are: totalBorderWidth = 2, edgePad = gap + border = 10, totalGap = gap + 2*border = 12 viewport = 1280 - 2*edgePad = 1260, height = 720 - 2*edgePad = 700 A 0.5 fraction column is then: round(0.5 * (viewport + totalGap)) - totalGap = round(636) - 12 = 624
readonly EXPECT_W=624
readonly EXPECT_H=700

spawn_client a
wait_for_windows 1

spawn_client b
wait_for_windows 2

windows=$("$UMBRIEL" windows --json)

if ! jq -e 'all(.[]; .floating == false)' <<< "$windows" > /dev/null; then
  echo "expected both windows tiled: $windows"
  exit 1
fi
if ! jq -e 'all(.[]; has("xwayland") and (.xwayland == false))' <<< "$windows" > /dev/null; then
  echo "expected native Wayland windows to report xwayland=false: $windows"
  exit 1
fi


if ! jq -e --argjson w "$EXPECT_W" 'all(.[]; .w == $w)' <<< "$windows" > /dev/null; then
  echo "expected both widths == $EXPECT_W (scrolling 0.5 fraction), got: $(jq -c '[.[].w]' <<< "$windows")"
  exit 1
fi

if ! jq -e --argjson h "$EXPECT_H" 'all(.[]; .h == $h)' <<< "$windows" > /dev/null; then
  echo "expected both heights == $EXPECT_H, got: $(jq -c '[.[].h]' <<< "$windows")"
  exit 1
fi

if ! jq -e '[.[].y] | unique | length == 1' <<< "$windows" > /dev/null; then
  echo "tiled columns should share a top edge, got: $(jq -c '[.[].y]' <<< "$windows")"
  exit 1
fi

if ! jq -e '[.[].x] | unique | length == 2' <<< "$windows" > /dev/null; then
  echo "two columns should occupy distinct x, got: $(jq -c '[.[].x]' <<< "$windows")"
  exit 1
fi

# Floating toggle round trip: the focused window flips and comes back.
"$UMBRIEL" msg window-toggle-floating > /dev/null
for _ in $(seq 20); do
  [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -eq 1 ]] && break
  sleep 0.2
done
if [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -ne 1 ]]; then
  echo "window-toggle-floating did not float the focused window: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg window-toggle-floating > /dev/null
for _ in $(seq 20); do
  [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -eq 0 ]] && break
  sleep 0.2
done
if [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -ne 0 ]]; then
  echo "window-toggle-floating did not re-tile the focused window: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "2 clients tiled at ${EXPECT_W}x${EXPECT_H}, float round trip ok"
