#!/usr/bin/env bash
# Closing the focused window while the overview is open re-focuses a survivor.
#
# The overview keeps the focus chrome while it owns the seat, so an unmap must
# move the workspace's focused view to a remaining window immediately: the card
# border is what shows where each row will land, and a dead focused view leaves
# no card highlighted until zoom-out happens to refocus. The closed window is
# closed through unmap-client, which unmaps on the close request without
# destroying the surface, so Server::removeView's destroy-time refocus can
# never mask a missing unmap-time reassignment.
set -euo pipefail

readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
CLIENT_PIDS=()
UNMAP_PID=

cleanup() {
  "$UMBRIEL" msg overview-close > /dev/null 2>&1 || true
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

spawn_survivor() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --title="overview-refocus-survivor" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want window(s), got $("$UMBRIEL" windows --json | jq 'length'): $("$UMBRIEL" windows --json)"
  return 1
}

# The survivor maps first so the unmap-client, mapping later, is the focused one.
spawn_survivor
wait_for_windows 1

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/unmap-client.log"
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  "$UNMAP_CLIENT" > "$CLIENT_LOG" 2>&1 &
UNMAP_PID=$!
CLIENT_PIDS+=("$UNMAP_PID")

for _ in $(seq 40); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "unmap-client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
wait_for_windows 2

unmap_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "unmap-client") | .id')
survivor_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-refocus-survivor") | .id')
if [[ -z $unmap_id || -z $survivor_id ]]; then
  echo "could not resolve both window ids: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The user-visible precondition: the newest window owns the focus.
if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$unmap_id" '.[] | select(.id == $id) | .focused') != true ]]; then
  echo "newly spawned unmap-client is not focused before the close"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6

"$UMBRIEL" msg "window-close:$unmap_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "unmap-client never received the close request: $(cat "$CLIENT_LOG")"
  exit 1
fi
if ! kill -0 "$UNMAP_PID" 2>/dev/null; then
  echo "unmap-client exited instead of staying alive, the check cannot distinguish unmap from destroy"
  exit 1
fi

# Focus must move to the survivor while the overview is still open.
focused=""
for _ in $(seq 40); do
  focused=$("$UMBRIEL" windows --json | jq -r --arg id "$survivor_id" '.[] | select(.id == $id) | .focused')
  [[ $focused == true ]] && break
  sleep 0.1
done
if [[ $focused != true ]]; then
  echo "survivor is not focused after closing the focused window in the overview: $("$UMBRIEL" windows --json)"
  exit 1
fi

# Zooming back in must land on the same window and keep it focused.
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6
if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$survivor_id" '.[] | select(.id == $id) | .focused') != true ]]; then
  echo "survivor lost focus after zooming out of the overview: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "closing the focused window in the overview re-focuses a survivor"
