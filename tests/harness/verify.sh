#!/usr/bin/env bash
# Boots a contained headless Umbriel, runs the checks in checks/, tears it down,
# and asserts the compositor exited cleanly.
#
# Containment matters. A stock Umbriel start runs its built-in autostarts, and
# `dbus-update-activation-environment --systemd` would repoint the *caller's*
# session-wide WAYLAND_DISPLAY and UMBRIEL_SOCKET at this throwaway instance.
# Unsetting DBUS_SESSION_BUS_ADDRESS makes both autostarts fail harmlessly.
#
# Usage: verify.sh <path-to-umbriel-binary> [check-name-filter]

set -euo pipefail

BINARY=${1:?usage: verify.sh <umbriel-binary> [filter]}
FILTER=${2:-}

if [[ ! -x $BINARY ]]; then
  echo "verify: '$BINARY' is not executable" >&2
  exit 1
fi
BINARY=$(realpath "$BINARY")
HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# sockaddr_un caps paths at 108 bytes and the compositor appends
# "/umbriel-wayland-0.sock" (23) to XDG_RUNTIME_DIR, so keep the root short.
# A long path makes wl_display_add_socket fail and the boot abort.
RUNTIME_DIR=$(mktemp -d /tmp/umv.XXXXXXXX)
LOG=$RUNTIME_DIR/compositor.log
CONFIG=$RUNTIME_DIR/config.toml
SERVER_PID=

cleanup() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

# No autostart, no xwayland, no cheatsheet: the harness wants a bare compositor,
# and each of those would spawn processes outside the container.
cat > "$CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF

echo "verify: booting $BINARY (runtime dir $RUNTIME_DIR)"
env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_HEADLESS_OUTPUTS=1 \
  "$BINARY" -c "$CONFIG" > "$LOG" 2>&1 &
SERVER_PID=$!

export UMBRIEL_SOCKET=$RUNTIME_DIR/umbriel-wayland-0.sock
for _ in $(seq 40); do
  [[ -S $UMBRIEL_SOCKET ]] && break
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "verify: compositor died during boot" >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
  fi
  sleep 0.25
done
if [[ ! -S $UMBRIEL_SOCKET ]]; then
  echo "verify: IPC socket never appeared" >&2
  sed 's/^/  | /' "$LOG" >&2
  exit 1
fi

export UMBRIEL=$BINARY
export UMBRIEL_RUNTIME_DIR=$RUNTIME_DIR
export UMBRIEL_LOG=$LOG
export UMBRIEL_CONFIG=$CONFIG

failed=0
ran=0
for check in "$HARNESS_DIR"/checks/*.sh; do
  name=$(basename "$check" .sh)
  if [[ -n $FILTER && $name != *"$FILTER"* ]]; then
    continue
  fi
  ran=$((ran + 1))
  if output=$(bash "$check" 2>&1); then
    echo "  ok   $name"
    [[ -n $output ]] && sed 's/^/         /' <<< "$output"
  else
    echo "  FAIL $name"
    sed 's/^/         /' <<< "$output"
    failed=$((failed + 1))
  fi
done

if [[ $ran -eq 0 ]]; then
  echo "verify: no checks matched filter '$FILTER'" >&2
  exit 1
fi

# Clean shutdown is itself a check: a listener still attached to a wlroots object
# at teardown trips an assert and the process dies on SIGABRT (exit 134) after
# having already logged "shutting down".
kill -TERM "$SERVER_PID"
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=

if [[ $status -ne 0 ]]; then
  echo "  FAIL shutdown (exit status $status, expected 0)"
  tail -5 "$LOG" | sed 's/^/         /'
  failed=$((failed + 1))
else
  echo "  ok   shutdown"
fi

if [[ $failed -gt 0 ]]; then
  echo "verify: $failed check(s) failed"
  exit 1
fi
echo "verify: all checks passed"
