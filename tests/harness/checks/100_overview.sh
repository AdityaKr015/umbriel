#!/usr/bin/env bash
# The overview steps workspaces from a wheel notch, and stops at the ends. While the overview is up the real window trees are hidden, so switching is a discrete step down the filmstrip rather than the animated slide it is outside. The wheel, the arrow keys and the three-finger swipe all reach that step through Overview::selectRelativeWorkspace. Only the wheel is drivable here: the headless backend has no touchpad, and zwlr_virtual_pointer_v1 carries motion, buttons and axes but no gesture events. So this covers the shared selection path; the gesture state machine on top of it is not reachable without a real device. Asserted through the log rather than through focus: with one window in the session, focus falls back to it whichever workspace is active, so focus does not distinguish a step that happened from one that did not.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi

readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.overview.bak"
CLIENT_PIDS=()
cleanup() {
  "$UMBRIEL" msg overview-close > /dev/null 2>&1 || true
  if [[ -f $CONFIG_BACKUP ]]; then
    cp "$CONFIG_BACKUP" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
  fi
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

pointer() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

# Lines appended to the log since $1, so each notch is judged on its own.
log_since() { tail -n +"$1" "$UMBRIEL_LOG"; }
log_mark() { wc -l < "$UMBRIEL_LOG"; }

# Send one notch and report which workspace it activated, or "none".
notch_activates() {
  local mark
  mark=$(($(log_mark) + 1))
  pointer notch "$1"
  for _ in $(seq 20); do
    local seen
    seen=$(log_since "$mark" | sed -n 's/.*activate workspace \([0-9]*\) on .*/\1/p' | tail -n 1)
    [[ -n $seen ]] && { echo "$seen"; return 0; }
    sleep 0.1
  done
  echo none
}

expect_notch() {
  local dir=$1 want=$2 got
  got=$(notch_activates "$dir")
  if [[ $got != "$want" ]]; then
    echo "notch $dir: expected workspace '$want', got '$got'"
    return 1
  fi
}

# One window, so the group holds workspace 1 (occupied) and a dynamic 2.
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
CLIENT_PIDS+=($!)
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "timed out waiting for the window to map"
  exit 1
fi

# Park the cursor over the output so the notch resolves to this group.
cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
pointer move $((OUTPUT_W / 2)) $((OUTPUT_H / 2))
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6

expect_notch 1 2  # down the filmstrip
expect_notch -1 1 # and back up

# At the top row there is nowhere further up: the step is refused rather than wrapping or running off the end of the group. This asserts the behaviour, not the
# bounds check that implements it: deleting that check still passes here, because workspaceAt() then returns null and select(null) is already a no-op.
if [[ $(notch_activates -1) != "none" ]]; then
  echo "a notch past the first workspace was not clamped"
  exit 1
fi

# Failed reloads keep both the committed config and live overview state.
printf '[layout\n' > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_notch 1 2
expect_notch -1 1

# A successful source change with no overview-invalidating runtime effect is
# equally inert. back_and_forth is read directly when switching workspaces.
{
  cat "$CONFIG_BACKUP"
  printf '\n[workspaces]\nback_and_forth = true\n'
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_notch 1 2
expect_notch -1 1

"$UMBRIEL" msg overview-close > /dev/null

echo "wheel steps survive failed and irrelevant reloads, and clamp at the top"
