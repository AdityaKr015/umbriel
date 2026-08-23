#!/usr/bin/env bash
# DPMS is compositor-owned output power, not output removal. A named action must
# affect only that monitor, a bare action must affect every configured monitor,
# and input activity must wake every monitor again.
set -euo pipefail

BINARY=$UMBRIEL
POINTER=${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}
RUNTIME_DIR=
SERVER_PID=
LOG=

cleanup() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -n $RUNTIME_DIR ]]; then
    rm -rf "$RUNTIME_DIR"
  fi
}
trap cleanup EXIT

RUNTIME_DIR=$(mktemp -d /tmp/umd.XXXXXXXX)
LOG=$RUNTIME_DIR/compositor.log
CONFIG=$RUNTIME_DIR/config.toml

cat > "$CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_HEADLESS_OUTPUTS=2 \
  "$BINARY" -c "$CONFIG" > "$LOG" 2>&1 &
SERVER_PID=$!

socket=$RUNTIME_DIR/umbriel-wayland-0.sock
for _ in $(seq 40); do
  [[ -S $socket ]] && break
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "compositor died during boot"
    sed 's/^/  | /' "$LOG"
    exit 1
  fi
  sleep 0.25
done
if [[ ! -S $socket ]]; then
  echo "IPC socket never appeared"
  sed 's/^/  | /' "$LOG"
  exit 1
fi
export UMBRIEL_SOCKET=$socket

log_mark() { wc -l < "$LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for log: $pattern"
  tail -12 "$LOG" | sed 's/^/  | /'
  return 1
}

# A named action changes only the requested monitor.
mark=$(log_mark)
"$BINARY" msg dpms-off:HEADLESS-2 >/dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"
if tail -n +"$((mark + 1))" "$LOG" | grep -q "output 'HEADLESS-1': powered off"; then
  echo "named DPMS action also powered off HEADLESS-1"
  exit 1
fi

mark=$(log_mark)
"$BINARY" msg dpms-on:HEADLESS-2 >/dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="

# A bare action changes all configured monitors.
mark=$(log_mark)
"$BINARY" msg dpms-off >/dev/null
wait_for_log_since "$mark" "output 'HEADLESS-1': powered off"
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"

# Pointer motion wakes both without an explicit dpms-on command.
mark=$(log_mark)
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
  "$POINTER" 2560 720 move 10 10
wait_for_log_since "$mark" "output 'HEADLESS-1': applied mode="
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="

echo "targeted and global DPMS actions preserve automatic input wake"
