#!/usr/bin/env bash
# The renderer's input color transform support enables the version 2 color-management global.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/global-client}"
readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
CLIENT_PID=

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
  if [[ -n $CLIENT_PID ]]; then
    kill -TERM "$CLIENT_PID" 2>/dev/null || true
  fi
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

env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  foot --title=color-diagnostics sh -c 'sleep 120' > /dev/null 2>&1 &
CLIENT_PID=$!
for _ in $(seq 40); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .color_manager == true
  and .renderer.input_color_transform == true
  and .renderer.output_color_transform == true
  and (.renderer.timeline | type) == "boolean"
  and (.outputs | length) == 1
  and .outputs[0].name == "HEADLESS-1"
  and .outputs[0].hdr_mode == "on"
  and .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == "display does not advertise PQ"
  and .outputs[0].render_format == "XR24"
  and .outputs[0].transfer_function == "none"
  and .outputs[0].primaries == "none"
  and .outputs[0].sdr_white == 203
  and (.outputs[0].supported_transfer_functions | type) == "array"
  and (.outputs[0].supported_primaries | type) == "array"
  and (.surfaces | length) == 1
  and .surfaces[0].title == "color-diagnostics"
  and .surfaces[0].transfer_function == "none"
  and .surfaces[0].primaries == "none"
  and .surfaces[0].mastering_display_primaries == null
  and .surfaces[0].mastering_luminance == null
  and .surfaces[0].max_cll == 0
  and .surfaces[0].max_fall == 0
' <<< "$color" > /dev/null; then
  echo "unexpected color diagnostics: $color"
  exit 1
fi

color_human=$("$UMBRIEL" color)
if ! grep -F "fallback: display does not advertise PQ" <<< "$color_human" > /dev/null \
    || ! grep -F "surface " <<< "$color_human" > /dev/null \
    || ! grep -F "mastering luminance: unset; MaxCLL: 0; MaxFALL: 0" <<< "$color_human" > /dev/null; then
  echo "unexpected human-readable color diagnostics: $color_human"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

cp "$UMBRIEL_CONFIG.color-management-bak" "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "auto"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_mode == "auto"
  and .outputs[0].hdr_requested == false
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == ""
' <<< "$color" > /dev/null; then
  echo "automatic HDR did not remain idle without HDR content: $color"
  exit 1
fi

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/auto-hdr-client.log"
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  REQUEST_FULLSCREEN=1 COLOR_HDR=1 \
  "$UNMAP_CLIENT" auto-hdr > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "automatic HDR client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "auto"
    and .outputs[0].hdr_requested == true
    and .outputs[0].hdr_active == false
    and .outputs[0].fallback_reason == "display does not advertise PQ"
    and (.surfaces[] | select(.title == "auto-hdr")
      | .transfer_function == "PQ" and .primaries == "BT.2020")
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].fallback_reason == "display does not advertise PQ"
  and (.surfaces[] | select(.title == "auto-hdr")
    | .transfer_function == "PQ" and .primaries == "BT.2020")
' <<< "$color" > /dev/null; then
  echo "fullscreen PQ content did not request automatic HDR: $color"
  exit 1
fi

auto_hdr_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "auto-hdr") | .id')
"$UMBRIEL" msg "window-close:$auto_hdr_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
color=$("$UMBRIEL" color --json)
if ! grep -q '^unmapped$' "$CLIENT_LOG" \
    || ! jq -e '
      .outputs[0].hdr_mode == "auto"
      and .outputs[0].hdr_requested == false
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == ""
    ' <<< "$color" > /dev/null; then
  echo "automatic HDR did not release after its owner unmapped: $color"
  exit 1
fi

echo "HDR diagnostics and fullscreen automatic HDR ownership transitions verified"
