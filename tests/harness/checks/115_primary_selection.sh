#!/usr/bin/env bash
# New clients must see the primary-selection global only while middle-click paste is enabled. This exercises the per-client protocol filter across live config
# reloads.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/global-client}"
readonly PRIMARY_SELECTION=zwp_primary_selection_device_manager_v1

if [[ ! -x $GLOBAL_CLIENT ]]; then
  echo "global client not built at $GLOBAL_CLIENT"
  exit 1
fi

query_global() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    "$GLOBAL_CLIENT" "$PRIMARY_SELECTION" "$1"
}

query_global present

cp "$UMBRIEL_CONFIG" "$UMBRIEL_CONFIG.primary-selection-bak"
cleanup() {
  mv "$UMBRIEL_CONFIG.primary-selection-bak" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
}
trap cleanup EXIT

printf '\n[input]\nmiddle_click_paste = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
query_global absent

sed -i 's/middle_click_paste = false/middle_click_paste = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
query_global present
