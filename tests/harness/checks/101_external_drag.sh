#!/usr/bin/env bash
# Opening the overview while a client owns a data-device drag must be refused.
# Otherwise the overview consumes the physical button release, leaving wlroots'
# pointer and keyboard drag grabs active indefinitely.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly DRAG_CLIENT="${UMBRIEL_DRAG_CLIENT:-./build-debug/drag-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/drag-client.log"
CLIENT_PIDS=()

cleanup() {
  "$UMBRIEL" msg overview-close > /dev/null 2>&1 || true
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi
if [[ ! -x $DRAG_CLIENT ]]; then
  echo "drag client not built at $DRAG_CLIENT"
  exit 1
fi

wayland_env=(
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR"
  WAYLAND_DISPLAY=wayland-0
)

# Ending a short drag without leaving its source surface must generate fresh
# pointer input. Clients use that input to restore the hover cursor which they
# replaced while dragging.
"${wayland_env[@]}" "$DRAG_CLIENT" cursor-refresh > "$CLIENT_LOG" 2>&1 &
DRAG_PID=$!
CLIENT_PIDS+=("$DRAG_PID")
for _ in $(seq 50); do
  grep -q '^ready$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^ready$' "$CLIENT_LOG"; then
  echo "timed out waiting for cursor refresh drag surface"
  exit 1
fi

"${wayland_env[@]}" "$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move 32 32 press "$LEFT_BUTTON" move 48 48 pause 100 release "$LEFT_BUTTON"
for _ in $(seq 50); do
  grep -q '^pointer-refreshed$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^pointer-refreshed$' "$CLIENT_LOG"; then
  echo "pointer input was not refreshed after the drag ended"
  cat "$CLIENT_LOG"
  exit 1
fi
wait "$DRAG_PID"
CLIENT_PIDS=()

: > "$CLIENT_LOG"

"${wayland_env[@]}" "$DRAG_CLIENT" > "$CLIENT_LOG" 2>&1 &
DRAG_PID=$!
CLIENT_PIDS+=("$DRAG_PID")
for _ in $(seq 50); do
  grep -q '^ready$' "$CLIENT_LOG" && break
  kill -0 "$DRAG_PID" 2>/dev/null || {
    echo "drag client exited before mapping"
    cat "$CLIENT_LOG"
    exit 1
  }
  sleep 0.1
done
if ! grep -q '^ready$' "$CLIENT_LOG"; then
  echo "timed out waiting for drag surface"
  exit 1
fi

# Press on the small top-left layer surface, move away from its passthrough
# region, hold long enough to request the overview, then physically release.
"${wayland_env[@]}" "$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move 32 32 press "$LEFT_BUTTON" move 640 360 pause 2000 release "$LEFT_BUTTON" &
POINTER_PID=$!
CLIENT_PIDS+=("$POINTER_PID")
for _ in $(seq 50); do
  grep -q '^drag-started$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^drag-started$' "$CLIENT_LOG"; then
  echo "timed out waiting for data-device drag"
  cat "$CLIENT_LOG"
  exit 1
fi

LOG_MARK=$(wc -l < "$UMBRIEL_LOG")
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.3
if tail -n +"$((LOG_MARK + 1))" "$UMBRIEL_LOG" | grep -q 'overview opened'; then
  echo "overview opened while a client drag was active"
  exit 1
fi

wait "$POINTER_PID"
for _ in $(seq 50); do
  grep -q '^drag-cancelled$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^drag-cancelled$' "$CLIENT_LOG"; then
  echo "physical release did not finish the client drag"
  cat "$CLIENT_LOG"
  exit 1
fi
wait "$DRAG_PID"
CLIENT_PIDS=()

# The refused request must not poison later overview activation.
LOG_MARK=$(wc -l < "$UMBRIEL_LOG")
"$UMBRIEL" msg overview-open > /dev/null
for _ in $(seq 20); do
  tail -n +"$((LOG_MARK + 1))" "$UMBRIEL_LOG" | grep -q 'overview opened' && break
  sleep 0.1
done
if ! tail -n +"$((LOG_MARK + 1))" "$UMBRIEL_LOG" | grep -q 'overview opened'; then
  echo "overview did not open after the client drag ended"
  exit 1
fi

# Panels remain interactive above the overview and can initiate their own
# client drag. Its release still belongs to the data-device grab, not a card.
: > "$CLIENT_LOG"
"${wayland_env[@]}" "$DRAG_CLIENT" > "$CLIENT_LOG" 2>&1 &
DRAG_PID=$!
CLIENT_PIDS=("$DRAG_PID")
for _ in $(seq 50); do
  grep -q '^ready$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^ready$' "$CLIENT_LOG"; then
  echo "timed out waiting for drag surface over overview"
  exit 1
fi

"${wayland_env[@]}" "$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move 32 32 press "$LEFT_BUTTON" move 640 360 pause 500 release "$LEFT_BUTTON" &
POINTER_PID=$!
CLIENT_PIDS+=("$POINTER_PID")
for _ in $(seq 50); do
  grep -q '^drag-started$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^drag-started$' "$CLIENT_LOG"; then
  echo "panel did not start a drag over the overview"
  exit 1
fi
wait "$POINTER_PID"
for _ in $(seq 50); do
  grep -q '^drag-cancelled$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^drag-cancelled$' "$CLIENT_LOG"; then
  echo "overview swallowed a panel drag release"
  exit 1
fi
wait "$DRAG_PID"
CLIENT_PIDS=()

"$UMBRIEL" msg overview-close > /dev/null
echo "client drags keep seat ownership across overview activation"
