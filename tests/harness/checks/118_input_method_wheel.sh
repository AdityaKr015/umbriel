#!/usr/bin/env bash
# An input method can own a virtual keyboard while grabbing events from physical keyboards. Modifier mouse and wheel
# binds must use the combined physical state, not the input method keyboard that happens to be current on the seat.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly INPUT_METHOD="${UMBRIEL_INPUT_METHOD_CLIENT:-./build-debug/input-method-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.input-method-wheel.bak"
readonly INPUT_METHOD_LOG="$UMBRIEL_RUNTIME_DIR/input-method.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/pointer.log"
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/input-first.log"
readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/input-second.log"
CLIENT_PIDS=()

cleanup() {
  for window_id in $(
    "$UMBRIEL" windows --json 2>/dev/null \
      | jq -r '.[] | select(.title == "input-first" or .title == "input-second") | .id' 2>/dev/null
  ); do
    "$UMBRIEL" msg "window-close:$window_id" > /dev/null 2>&1 || true
  done
  for _ in $(seq 40); do
    [[ $("$UMBRIEL" windows --json 2>/dev/null | jq 'length' 2>/dev/null) -eq 0 ]] && break
    sleep 0.05
  done
  if [[ -f $CONFIG_BACKUP ]]; then
    cp "$CONFIG_BACKUP" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
  fi
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
  for pid in "${CLIENT_PIDS[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

wayland_env() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 "$@"
}

spawn_client() {
  wayland_env "$CLIENT" "$1" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  CLIENT_PIDS+=($!)
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

active_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1

[keybinds]
"Mod+WheelDown" = "workspace-next"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "input-first"
wait_for_count 1
"$UMBRIEL" msg workspace-switch:2 > /dev/null
sleep 0.1
spawn_client "input-second"
wait_for_count 2
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.1

# Create the physical harness keyboard first, then pause while the input method creates its own current keyboard and
# grabs the physical one. This is the ordering used by input methods that inject composed text through a virtual device.
wayland_env "$POINTER" "$OUTPUT_W" "$OUTPUT_H" pause 1000 mod logo notch 1 mod none tap 30 > "$POINTER_LOG" 2>&1 &
CLIENT_PIDS+=($!)
sleep 0.1
wayland_env "$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
CLIENT_PIDS+=($!)

for _ in $(seq 40); do
  grep -qx 'grabbed' "$INPUT_METHOD_LOG" 2>/dev/null && break
  sleep 0.05
done
if ! grep -qx 'grabbed' "$INPUT_METHOD_LOG" 2>/dev/null; then
  echo "input method did not establish its keyboard grab: $(< "$INPUT_METHOD_LOG")"
  exit 1
fi

wait "${CLIENT_PIDS[2]}" || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}
if [[ $(active_title) != input-second ]]; then
  echo "Mod+WheelDown ignored the physical modifier during an input-method grab: $("$UMBRIEL" windows --json)"
  exit 1
fi

for _ in $(seq 40); do
  grep -q '^key 30 1$' "$SECOND_LOG" 2>/dev/null && break
  sleep 0.05
done
if ! grep -q '^key 30 1$' "$SECOND_LOG" 2>/dev/null; then
  echo "keyboard input did not follow focus to workspace 2: first=$(< "$FIRST_LOG") second=$(< "$SECOND_LOG")"
  exit 1
fi
if grep -q '^key 30 1$' "$FIRST_LOG" 2>/dev/null; then
  echo "keyboard input remained on workspace 1 after switching: first=$(< "$FIRST_LOG") second=$(< "$SECOND_LOG")"
  exit 1
fi

echo "input-method grabs preserve modifier wheel bindings and keyboard focus"
