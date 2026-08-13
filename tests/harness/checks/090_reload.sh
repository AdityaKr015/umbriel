#!/usr/bin/env bash
# A reload that changes nothing does nothing; one that changes something applies
# only that.
#
# Reload used to re-apply every subsystem unconditionally, which is visible: the
# view loop clears every focus ring before refocus puts one back, so a no-op
# reload flickers and can land focus somewhere else entirely. This asserts the
# two halves of the fix -- that an unchanged file is inert, and that a changed
# one still takes effect.
set -euo pipefail

CLIENT_PIDS=()
cleanup() {
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

spawn_client() {
  env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
    foot sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

snapshot() { "$UMBRIEL" windows --json | jq -Sc '[.[] | {w, h, x, focused}] | sort_by(.x)'; }

settle() {
  local previous="" current=""
  for _ in $(seq 40); do
    current=$(snapshot)
    [[ -n $previous && $current == "$previous" ]] && return 0
    previous=$current
    sleep 0.25
  done
  echo "state never settled (last: $previous)"
  return 1
}

spawn_client
spawn_client
wait_for_count 2 || exit 1
settle || exit 1

before=$(snapshot)

# Rewrite the file with identical content: mtime moves, content does not.
cp "$UMBRIEL_CONFIG" "$UMBRIEL_CONFIG.bak"
cat "$UMBRIEL_CONFIG.bak" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
settle || exit 1

after=$(snapshot)
if [[ $before != "$after" ]]; then
  echo "a no-op reload changed the session"
  echo "  before: $before"
  echo "  after:  $after"
  exit 1
fi

# Window state alone cannot tell the two apart: re-applying everything lands on
# the same geometry and refocuses the same window. What it cannot fake is the
# reload reporting that it had nothing to do.
if ! tail -n 20 "$UMBRIEL_LOG" | grep -q "config reloaded (no changes)"; then
  echo "a no-op reload reported work:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi

# A real change must still land. Gaps feed the layout, so the tiles move.
{
  cat "$UMBRIEL_CONFIG.bak"
  printf '\n[layout]\ngap = 40\n'
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
settle || exit 1

changed=$(snapshot)
if [[ $changed == "$after" ]]; then
  echo "a gap change did not reach the layout (still $changed)"
  exit 1
fi
if ! tail -n 20 "$UMBRIEL_LOG" | grep -q "config reloaded (layout)"; then
  echo "a gap change was not reported as a layout change:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi

echo "         no-op reload inert; gap change applied"
