#!/usr/bin/env bash
# harness: outputs=2
# DPMS is compositor-owned output power, not output removal. A named action must
# affect only that monitor, a bare action must affect every configured monitor,
# and input activity must wake every monitor again. Two monitors are the whole
# point, which the header directive above asks the harness for.
set -euo pipefail

POINTER=${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for log: $pattern"
  tail -12 "$UMBRIEL_LOG" | sed 's/^/  | /'
  return 1
}

# A named action changes only the requested monitor.
mark=$(log_mark)
"$UMBRIEL" msg dpms-off:HEADLESS-2 > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"
if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "output 'HEADLESS-1': powered off"; then
  echo "named DPMS action also powered off HEADLESS-1"
  exit 1
fi

mark=$(log_mark)
"$UMBRIEL" msg dpms-on:HEADLESS-2 > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="

# A bare action changes all configured monitors.
mark=$(log_mark)
"$UMBRIEL" msg dpms-off > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-1': powered off"
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"

# Pointer motion wakes both without an explicit dpms-on command.
mark=$(log_mark)
"$POINTER" 2560 720 move 10 10
wait_for_log_since "$mark" "output 'HEADLESS-1': applied mode="
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="

echo "targeted and global DPMS actions preserve automatic input wake"
