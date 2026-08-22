#!/usr/bin/env bash
# A client commit must not discard a compositor-owned window-rule opacity. This
# reproduces browsers that continually redraw after a workspace transition.
set -euo pipefail

readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.rule-opacity-commit.bak"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-opacity-commit.png"
CLIENT_PID=

cleanup() {
  if [[ -f $CONFIG_BACKUP ]]; then
    cp "$CONFIG_BACKUP" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
  fi
  if [[ -n $CLIENT_PID ]]; then
    kill -KILL "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1
border_width = 0
corner_radius = 0
backdrop_color = "#00FF00FF"

[[window_rule]]
match.app_id = "^opacity-commit$"
opacity = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  foot --config=/dev/null --app-id=opacity-commit --override=colors.background=000000 \
  sh -c 'while :; do printf "\\r%08d" "$RANDOM"; sleep 0.02; done' > /dev/null 2>&1 &
CLIENT_PID=$!

for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "timed out waiting for opacity-commit"
  exit 1
fi

# Switching back applies the view clip, then subsequent client commits must
# preserve the 0.5 compositor opacity rather than restoring opaque black.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.5
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 grim "$SCREENSHOT"

# The sampled pixel is black client content over the solid green compositor
# backdrop. At 0.5 opacity, its encoded green channel is about 128. Opaque
# black, the broken post-commit state, yields zero.
green=$(magick "$SCREENSHOT" -crop 40x40+600+500 -format '%[fx:round(255*mean.g)]' info:)
if (( green < 90 || green > 170 )); then
  echo "rule opacity was lost after the client commit: mean green=$green"
  exit 1
fi

echo "rule opacity survived a post-workspace-switch client commit: mean green=$green"
