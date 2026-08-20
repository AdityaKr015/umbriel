#!/usr/bin/env bash
# The config `enabled` key must disable an output live: the commit turns the
# monitor off and the output leaves the layout. Re-enabling restores it.
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

# Disable the output through a live reload.
mark=$(log_mark)
cat > "$UMBRIEL_CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[output.HEADLESS-1]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-1': disabled by config"; then
  echo "output was not disabled on reload"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
fi

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
