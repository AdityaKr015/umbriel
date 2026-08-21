#!/usr/bin/env bash
# session-quit asks for confirmation; session-quit:skip-confirmation does not. The shared harness instance must survive, so this check boots its own headless compositor (same containment as verify.sh): plain session-quit must leave it answering IPC, a second session-quit must terminate it cleanly, and the bypass form must terminate a fresh instance cleanly in one call.
set -euo pipefail

BINARY=$UMBRIEL
RUNTIME_DIR=
SERVER_PID=

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

boot_instance() {
  # sockaddr_un caps paths at 108 bytes and the compositor appends
  # "/umbriel-wayland-0.sock" (23) to XDG_RUNTIME_DIR, so keep the root short.
  RUNTIME_DIR=$(mktemp -d /tmp/umq.XXXXXXXX)
  local log=$RUNTIME_DIR/compositor.log
  local config=$RUNTIME_DIR/config.toml

  cat > "$config" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF

  env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    WLR_BACKENDS=headless \
    WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_HEADLESS_OUTPUTS=1 \
    "$BINARY" -c "$config" > "$log" 2>&1 &
  SERVER_PID=$!

  local socket=$RUNTIME_DIR/umbriel-wayland-0.sock
  for _ in $(seq 40); do
    [[ -S $socket ]] && break
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "compositor died during boot" >&2
      sed 's/^/  | /' "$log" >&2
      return 1
    fi
    sleep 0.25
  done
  if [[ ! -S $socket ]]; then
    echo "IPC socket never appeared" >&2
    sed 's/^/  | /' "$log" >&2
    return 1
  fi
  export UMBRIEL_SOCKET=$socket
}

# The first invocation opens the dialog and must NOT terminate the compositor.
boot_instance
if ! "$BINARY" msg session-quit > /dev/null 2>&1; then
  echo "msg session-quit was rejected"
  exit 1
fi
sleep 0.5
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "compositor exited on a single session-quit"
  exit 1
fi
if ! "$BINARY" windows > /dev/null 2>&1; then
  echo "compositor stopped answering IPC after session-quit"
  exit 1
fi

# A second invocation confirms and shuts down cleanly. The IPC
# reply may be lost as the display terminates, so tolerate a failed msg.
"$BINARY" msg session-quit > /dev/null 2>&1 || true
for _ in $(seq 40); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  sleep 0.25
done
if kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "compositor survived a second session-quit"
  exit 1
fi
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=
if [[ $status -ne 0 ]]; then
  echo "compositor exited with status $status on confirm, expected 0"
  exit 1
fi

# The bypass form terminates a fresh instance in a single call.
boot_instance
"$BINARY" msg session-quit:skip-confirmation > /dev/null 2>&1 || true
for _ in $(seq 40); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  sleep 0.25
done
if kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "compositor survived session-quit:skip-confirmation"
  exit 1
fi
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=
if [[ $status -ne 0 ]]; then
  echo "compositor exited with status $status on bypass, expected 0"
  exit 1
fi

echo "session-quit confirms; skip-confirmation quits immediately"
