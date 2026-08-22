#!/usr/bin/env bash
# Two-output containment check. Not part of verify.sh: that harness boots one headless output and most of its checks
# assume it, so this boots its own two-output instance. It scrolls one output's strip until columns hang off the shared
# edge, physically on top of the neighbour, then asserts from real framebuffers that the neighbour is byte-identical to
# its empty baseline (nothing bled), that the home output did change (content is drawn), and that content still reaches
# the right side of the home strip (the per-output clip did not cut it short).
# Usage: two-output-containment.sh <umbriel-binary>
set -euo pipefail

BINARY=$(realpath "${1:?usage: two-output-containment.sh <umbriel-binary>}")
RUNTIME_DIR=$(mktemp -d /tmp/umb2.XXXXXXXX)
CONFIG=$RUNTIME_DIR/config.toml
LOG=$RUNTIME_DIR/compositor.log
CLIENTS=()
rc=0

cleanup() {
  for pid in "${CLIENTS[@]:-}"; do
    [[ -n $pid ]] && kill -TERM "$pid" 2>/dev/null || true
  done
  [[ -n ${SERVER_PID:-} ]] && kill -KILL "$SERVER_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

cat > "$CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=2 \
  "$BINARY" -c "$CONFIG" > "$LOG" 2>&1 &
SERVER_PID=$!

export UMBRIEL_SOCKET=$RUNTIME_DIR/umbriel-wayland-0.sock
for _ in $(seq 40); do
  [[ -S $UMBRIEL_SOCKET ]] && break
  sleep 0.25
done
[[ -S $UMBRIEL_SOCKET ]] || { echo "compositor never came up"; sed 's/^/  | /' "$LOG"; exit 1; }

umb() { env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$RUNTIME_DIR" "$BINARY" "$@"; }
shot() { env -u DISPLAY XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 grim -o "$1" "$2"; }
spawn() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --title="$1" sh -c 'sleep 300' > /dev/null 2>&1 &
  CLIENTS+=($!)
}
wait_windows() {
  for _ in $(seq 60); do
    [[ $(umb windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "expected $1 window(s), got $(umb windows --json | jq 'length')"
  return 1
}
output_x() {
  umb outputs | awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}
# Mean of a crop, as a stable fingerprint of one screen region.
region_mean() {
  magick "$1" -crop "$2" -colorspace RGB -format '%[fx:mean]' info:
}
check() {
  if [[ $2 == "$3" ]]; then
    echo "  ok   $1"
  else
    echo "  FAIL $1 (got '$2', want '$3')"
    rc=1
  fi
}

spawn probe
wait_windows 1
home=$(umb windows --json | jq -r '.[0].workspace' | cut -d: -f1)
if [[ $home == HEADLESS-1 ]]; then neighbour=HEADLESS-2; else neighbour=HEADLESS-1; fi
home_x=$(output_x "$home")
echo "windows land on $home (x=$home_x), watching $neighbour (x=$(output_x "$neighbour"))"

sleep 1
shot "$neighbour" "$RUNTIME_DIR/neighbour-base.png"
shot "$home" "$RUNTIME_DIR/home-base.png"
# The rightmost on-strip column ends just short of the shared edge, so this crop is window content when the strip is
# populated and backdrop when it is empty.
edge_crop=20x600+1246+60
home_base_edge=$(region_mean "$RUNTIME_DIR/home-base.png" "$edge_crop")

# 624-wide columns on a 1280-wide output fit two at a time. Focusing the leftmost column scrolls the surplus off the
# RIGHT edge, which in a side-by-side layout is exactly where the neighbouring output lives.
for i in 2 3 4 5; do spawn "bleed-$i"; done
wait_windows 5
for _ in $(seq 6); do umb msg window-focus-left > /dev/null; sleep 0.2; done
sleep 2

edge=$((home_x + 1280))
umb windows --json | jq -c '[.[] | {title, x, w}] | sort_by(.x)'
over=$(umb windows --json | jq --argjson e "$edge" '[.[] | select(.x + .w > $e)] | length')
echo "columns reaching past the shared edge at x=$edge: $over"
[[ $over -gt 0 ]] || { echo "SETUP FAIL: nothing reaches past the shared edge"; exit 1; }

shot "$neighbour" "$RUNTIME_DIR/neighbour-full.png"
shot "$home" "$RUNTIME_DIR/home-full.png"

neighbour_same=$(cmp -s "$RUNTIME_DIR/neighbour-base.png" "$RUNTIME_DIR/neighbour-full.png" && echo same || echo changed)
home_same=$(cmp -s "$RUNTIME_DIR/home-base.png" "$RUNTIME_DIR/home-full.png" && echo same || echo changed)
home_full_edge=$(region_mean "$RUNTIME_DIR/home-full.png" "$edge_crop")
edge_state=$([[ $home_base_edge == "$home_full_edge" ]] && echo same || echo changed)

check "no bleed onto $neighbour" "$neighbour_same" same
check "content drawn on $home" "$home_same" changed
check "content reaches the strip edge on the home output" "$edge_state" changed

# Transitions are where containment used to be re-derived per move: a workspace slide, a fullscreen enter/leave, and a
# focus scroll all move nodes while columns hang over the neighbour. Sample the neighbour after each step; it must never
# change, including mid-animation.
transition_drift=0
for action in window-toggle-fullscreen window-toggle-fullscreen workspace-next workspace-previous \
  window-focus-right window-focus-left column-move-right column-move-left; do
  umb msg "$action" > /dev/null
  for _ in 1 2 3; do
    sleep 0.08
    shot "$neighbour" "$RUNTIME_DIR/neighbour-step.png"
    if ! cmp -s "$RUNTIME_DIR/neighbour-base.png" "$RUNTIME_DIR/neighbour-step.png"; then
      echo "  drift after $action"
      transition_drift=$((transition_drift + 1))
      cp "$RUNTIME_DIR/neighbour-step.png" "$RUNTIME_DIR/drift-$action.png"
      break
    fi
  done
done
check "no bleed onto $neighbour across transitions" "$transition_drift" 0

if [[ $rc -ne 0 ]]; then
  cp "$RUNTIME_DIR"/*.png /tmp/ 2>/dev/null || true
  echo "  images copied to /tmp"
fi
exit "$rc"
