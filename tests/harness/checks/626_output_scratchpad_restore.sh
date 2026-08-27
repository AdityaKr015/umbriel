#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad window is parked on an output rather than on a workspace, and it has to find its way back to that output
# after every monitor goes away and returns.
set -euo pipefail

BASELINE=$(< "$UMBRIEL_CONFIG")

write_config() {
  {
    printf '%s\n' "$BASELINE"
    if [[ -n $1 ]]; then
      printf '\n%s\n' "$1"
    fi
  } > "$UMBRIEL_CONFIG"
}

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

expect_log_since() {
  local mark=$1 pattern=$2 message=$3
  if wait_for_log_since "$mark" "$pattern"; then
    return 0
  fi
  echo "$message"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
}

reload_with() {
  local mark=
  mark=$(log_mark)
  write_config "$1"
  "$UMBRIEL" msg config-reload > /dev/null
  shift
  for expected in "$@"; do
    expect_log_since "$mark" "${expected#*=}" "output ${expected%%=*} did not ${expected#*=} on reload"
  done
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.25
  done
  echo "expected $expected window(s), got $count"
  return 1
}

output_origin() {
  "$UMBRIEL" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1], p[2]; exit }'
}

scratchpad_offset() {
  local output=$1 window_x window_y output_x output_y
  read -r window_x window_y < <(
    "$UMBRIEL" windows --json \
      | jq -r '.[] | select(.title == "scratch-restore") | "\(.x) \(.y)"'
  )
  read -r output_x output_y < <(output_origin "$output")
  printf '{"x":%d,"y":%d}' "$((window_x - output_x))" "$((window_y - output_y))"
}

# The drawer only opens on the output holding the window, and focuses it when it does. Closed again either way.
shows_scratchpad() {
  local output=$1 active=
  "$UMBRIEL" msg "scratchpad-toggle:$output" > /dev/null
  for _ in $(seq 10); do
    active=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "scratch-restore") | .active')
    [[ $active == true ]] && break
    sleep 0.1
  done
  "$UMBRIEL" msg "scratchpad-toggle:$output" > /dev/null
  [[ $active == true ]]
}

write_config '[animation.scratchpad]
scale = 0

[[window_rule]]
match.title = "^scratch-restore$"
default_output = "HEADLESS-2"
default_floating = true
default_size = [420, 260]
default_position = { x = 91, y = 73, anchor = "top_left" }'
"$UMBRIEL" msg config-reload > /dev/null

foot --title=scratch-restore sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_count 1
"$UMBRIEL" msg window-move-to-scratchpad:HEADLESS-2 > /dev/null
if ! shows_scratchpad HEADLESS-2; then
  echo "window was not parked on the HEADLESS-2 scratchpad"
  exit 1
fi
before=$(scratchpad_offset HEADLESS-2)
expected='{"x":91,"y":73}'
if [[ $before != "$expected" ]]; then
  echo "scratchpad did not start at the configured off-center position: expected=$expected got=$before"
  exit 1
fi

# Every monitor goes away at once, the way a suspend takes them.
reload_with '[output.HEADLESS-1]
enabled = false
[output.HEADLESS-2]
enabled = false' "HEADLESS-1=output 'HEADLESS-1': disabled by config" "HEADLESS-2=output 'HEADLESS-2': disabled by config"

# HEADLESS-1 comes back first and takes the window in, though it is not where it belongs.
reload_with '[output.HEADLESS-2]
enabled = false' "HEADLESS-1=output 'HEADLESS-1': applied mode="
if ! shows_scratchpad HEADLESS-1; then
  echo "scratchpad window was not rescued onto the one output that came back"
  exit 1
fi

# HEADLESS-2 comes back and takes its window back.
reload_with '' "HEADLESS-2=output 'HEADLESS-2': applied mode="
if ! shows_scratchpad HEADLESS-2; then
  echo "scratchpad window did not go back to HEADLESS-2"
  exit 1
fi
if shows_scratchpad HEADLESS-1; then
  echo "scratchpad window is still on HEADLESS-1 as well"
  exit 1
fi
after=$(scratchpad_offset HEADLESS-2)
if [[ $after != "$before" ]]; then
  echo "scratchpad output-relative position changed across restoration: before=$before after=$after"
  exit 1
fi

echo "a scratchpad window was rescued while its output was gone, then returned with its position intact"
