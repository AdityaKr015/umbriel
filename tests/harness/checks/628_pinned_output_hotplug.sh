#!/usr/bin/env bash
# A globally pinned view hangs below an output-owned clipping root while an output exists. Destroying the last output
# must park that view on the server-owned pinned roots before the output roots disappear. A client redraw after the
# removal catches stale scene-node ownership immediately, and recreating the output proves the pinned view can be
# rehomed afterwards.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/pinned-output-hotplug-client.log"

env REDRAW_ON_CLOSE=1 "$CLIENT" pinned-output-hotplug 1200 700 > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "pinned output hotplug client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "pinned-output-hotplug") | .id')
if [[ -z $window_id ]]; then
  echo "could not resolve the pinned window id"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
"$UMBRIEL" msg window-toggle-pinned > /dev/null

"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" msg "window-close:$window_id" > /dev/null
for _ in $(seq 60); do
  grep -q '^redrawn$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^redrawn$' "$CLIENT_LOG"; then
  echo "pinned client did not redraw after its output disappeared: $(cat "$CLIENT_LOG")"
  exit 1
fi

created=$("$UMBRIEL" output-create HEADLESS-1)
if [[ $created != HEADLESS-1 ]]; then
  echo "expected the recreated output to be named HEADLESS-1, got '$created'"
  exit 1
fi

workspace=
for _ in $(seq 60); do
  workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$window_id" '.[] | select(.id == $id) | .workspace')
  [[ $workspace == HEADLESS-1:* ]] && break
  sleep 0.1
done
if [[ $workspace != HEADLESS-1:* ]]; then
  echo "pinned window did not return to the recreated output: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "a pinned window survived its output being destroyed, a redraw, and output recreation"
