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
