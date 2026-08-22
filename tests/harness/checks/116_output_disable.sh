#!/usr/bin/env bash
# The config `enabled` key must disable an output live: the commit turns the monitor off, its windows move to a live
# output, and re-enabling restores it. That needs two outputs, and the shared harness instance is deliberately
# single-output because 055 asserts every directional output action is rejected there. So this check boots its own
# two-output compositor with the same containment as verify.sh rather than making the shared one ambiguous.
set -euo pipefail

BINARY=$UMBRIEL
RUNTIME_DIR=
SERVER_PID=
CLIENT_PID=
LOG=
CONFIG=

cleanup() {
  if [[ -n $CLIENT_PID ]]; then
    kill -TERM "$CLIENT_PID" 2>/dev/null || true
  fi
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
  RUNTIME_DIR=$(mktemp -d /tmp/umo.XXXXXXXX)
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

  local socket=$RUNTIME_DIR/umbriel-wayland-0.sock
  for _ in $(seq 40); do
    [[ -S $socket ]] && break
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "compositor died during boot" >&2
      sed 's/^/  | /' "$LOG" >&2
      return 1
    fi
    sleep 0.25
  done
  if [[ ! -S $socket ]]; then
    echo "IPC socket never appeared" >&2
    sed 's/^/  | /' "$LOG" >&2
    return 1
  fi
  export UMBRIEL_SOCKET=$socket
}

log_mark() { wc -l < "$LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

spawn_client() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --title=output-disable-rehome sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PID=$!
}

wait_for_workspace() {
  local expected=$1 workspace= windows=
  for _ in $(seq 40); do
    windows=$("$BINARY" windows --json)
    if [[ $(jq 'length' <<< "$windows") -ne 1 ]]; then
      sleep 0.1
      continue
    fi
    workspace=$(jq -r '.[0].workspace' <<< "$windows")
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected window workspace '$expected', got '$workspace'"
  return 1
}

boot_instance
spawn_client
wait_for_workspace 'HEADLESS-2:1'

# Disable the output through a live reload.
mark=$(log_mark)
cat > "$CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[output.HEADLESS-2]
enabled = false
EOF
"$BINARY" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': disabled by config"; then
  echo "output was not disabled on reload"
  tail -8 "$LOG" | sed 's/^/  | /'
  exit 1
fi
wait_for_workspace 'HEADLESS-1:1'

# Re-enable: the compositor must survive the disable and come back with a mode.
mark=$(log_mark)
cat > "$CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF
"$BINARY" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="; then
  echo "output was not re-enabled on reload"
  tail -8 "$LOG" | sed 's/^/  | /'
  exit 1
fi

echo "output disabled and re-enabled through live reload"
