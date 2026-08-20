#!/usr/bin/env bash
# The config `enabled` key must disable an output live: the commit turns the
# monitor off, its windows move to a live output, and re-enabling restores it.
set -euo pipefail

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

spawn_client() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot --title=output-disable-rehome sh -c 'sleep 120' > /dev/null 2>&1 &
  client_pid=$!
}

cleanup() {
  if [[ -n ${client_pid:-} ]]; then
    kill -TERM "$client_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

wait_for_workspace() {
  local expected=$1 workspace= windows=
  for _ in $(seq 40); do
    windows=$("$UMBRIEL" windows --json)
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

spawn_client
wait_for_workspace 'HEADLESS-2:1'

# Disable the output through a live reload.
mark=$(log_mark)
cat > "$UMBRIEL_CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[output.HEADLESS-2]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': disabled by config"; then
  echo "output was not disabled on reload"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
fi
wait_for_workspace 'HEADLESS-1:1'

# Re-enable: the compositor must survive the disable and come back with a mode.
mark=$(log_mark)
cat > "$UMBRIEL_CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-1': applied mode="; then
  echo "output was not re-enabled on reload"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
fi

echo "         output disabled and re-enabled through live reload"
