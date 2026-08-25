#!/usr/bin/env bash
# Closing the focused Dwindle window must focus its layout predecessor even when focus follows the mouse and the
# stationary pointer rests over another survivor. The closing client stays alive after unmapping, so the assertion
# observes the unmap transition itself instead of relying on destroy-time fallback focus.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  local title=$1
  local log=$2
  "$CLIENT" "$title" 1200 700 > "$log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_focus() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$want" '.[] | select(.id == $id) | .focused') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to be focused: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "dwindle-close-pointer" "$UMBRIEL_RUNTIME_DIR/pointer-window.log"
wait_for_count 1
spawn_client "dwindle-close-predecessor" "$UMBRIEL_RUNTIME_DIR/predecessor-window.log"
wait_for_count 2
spawn_client "dwindle-close-focused" "$UMBRIEL_RUNTIME_DIR/focused-window.log"
wait_for_count 3

windows=$("$UMBRIEL" windows --json)
pointer_id=$(jq -r '.[] | select(.title == "dwindle-close-pointer") | .id' <<< "$windows")
predecessor_id=$(jq -r '.[] | select(.title == "dwindle-close-predecessor") | .id' <<< "$windows")
focused_id=$(jq -r '.[] | select(.title == "dwindle-close-focused") | .id' <<< "$windows")
pointer_x=$(jq -r '.[] | select(.title == "dwindle-close-pointer") | (.x + .w / 2 | round)' <<< "$windows")
pointer_y=$(jq -r '.[] | select(.title == "dwindle-close-pointer") | (.y + .h / 2 | round)' <<< "$windows")
if [[ -z $pointer_id || -z $predecessor_id || -z $focused_id ]]; then
  echo "could not resolve Dwindle window ids: $windows"
  exit 1
fi

# Establish the conflicting candidates: the pointer rests over the first window, while the newest window owns focus.
pointer move "$pointer_x" "$pointer_y"
wait_for_focus "$pointer_id"
"$UMBRIEL" msg "window-focus:$focused_id" > /dev/null
wait_for_focus "$focused_id"

"$UMBRIEL" msg "window-close:$focused_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/focused-window.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/focused-window.log"; then
  echo "focused client did not unmap: $(< "$UMBRIEL_RUNTIME_DIR/focused-window.log")"
  exit 1
fi

wait_for_focus "$predecessor_id"
echo "closing a focused Dwindle window ignores a stationary pointer and focuses its predecessor"
