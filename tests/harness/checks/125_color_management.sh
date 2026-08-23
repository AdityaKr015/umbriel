#!/usr/bin/env bash
# The renderer's input color transform support enables the version 2 color-management global.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/global-client}"

if [[ ! -x $GLOBAL_CLIENT ]]; then
  echo "global client not built at $GLOBAL_CLIENT"
  exit 1
fi

env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  "$GLOBAL_CLIENT" wp_color_manager_v1 present 2

echo "wp_color_manager_v1 version 2 advertised"

cp "$UMBRIEL_CONFIG" "$UMBRIEL_CONFIG.color-management-bak"
cleanup() {
  mv "$UMBRIEL_CONFIG.color-management-bak" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
}
trap cleanup EXIT

printf '\n[output.HEADLESS-1]\nhdr = "on"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

if ! grep -F "output 'HEADLESS-1': HDR unavailable: display does not advertise PQ" "$UMBRIEL_LOG" > /dev/null; then
  echo "missing expected headless HDR fallback: display does not advertise PQ"
  exit 1
fi
echo "headless HDR request reports missing PQ support"
