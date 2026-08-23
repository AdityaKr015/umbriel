#!/usr/bin/env bash
# Boots a contained headless Umbriel, runs the checks in checks/, tears it down, and asserts the compositor exited cleanly. Containment matters. A stock Umbriel start runs its built-in autostarts, and `dbus-update-activation-environment --systemd` would repoint the *caller's* session-wide WAYLAND_DISPLAY and UMBRIEL_SOCKET at this throwaway instance. Unsetting DBUS_SESSION_BUS_ADDRESS makes both autostarts fail harmlessly.
# Usage: verify.sh <path-to-umbriel-binary> [name-fragment ...] [-v|--verbose] [-l|--list]
# Each name fragment selects every check whose name contains it, so several fragments run several checks. Without a
# fragment the whole suite runs. Failing runs keep their runtime directory (compositor and client logs) and print it.

set -euo pipefail

BINARY=${1:?usage: verify.sh <umbriel-binary> [name-fragment ...] [-v] [-l]}
shift

FILTERS=()
VERBOSE=${VERIFY_VERBOSE:-0}
LIST_ONLY=0
for arg in "$@"; do
  case $arg in
    -v | --verbose) VERBOSE=1 ;;
    -l | --list) LIST_ONLY=1 ;;
    # `just verify debug` forwards an empty filter; treat it as "no filter".
    '') ;;
    -*)
      echo "verify: unknown option '$arg'" >&2
      exit 2
      ;;
    *) FILTERS+=("$arg") ;;
  esac
done

HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# Colour only for a terminal, so piped output and CI logs stay plain text.
if [[ -t 1 && -z ${NO_COLOR:-} && ${TERM:-dumb} != dumb ]]; then
  TTY=1
  C_OFF=$'\e[0m'
  C_DIM=$'\e[2m'
  C_BOLD=$'\e[1m'
  C_PASS=$'\e[32m'
  C_FAIL=$'\e[31m'
  C_RUN=$'\e[33m'
else
  TTY=0
  C_OFF='' C_DIM='' C_BOLD='' C_PASS='' C_FAIL='' C_RUN=''
fi
readonly NAME_WIDTH=34
COLUMNS_MAX=${COLUMNS:-100}
[[ $COLUMNS_MAX -lt 60 ]] && COLUMNS_MAX=60

all_checks() {
  local check
  for check in "$HARNESS_DIR"/checks/*.sh; do
    basename "$check" .sh
  done
}

selects() {
  local name=$1 filter
  ((${#FILTERS[@]} == 0)) && return 0
  for filter in "${FILTERS[@]}"; do
    [[ $name == *"$filter"* ]] && return 0
  done
  return 1
}

if ((LIST_ONLY)); then
  while read -r name; do
    selects "$name" && echo "$name"
  done <<< "$(all_checks)"
  exit 0
fi

# Select before booting: an unmatched fragment is a typo, and reporting it costs
# nothing when no compositor is running yet.
SELECTED=()
while read -r name; do
  selects "$name" && SELECTED+=("$name")
done <<< "$(all_checks)"
TOTAL=$(all_checks | wc -l)
if ((${#SELECTED[@]} == 0)); then
  echo "verify: no checks matched ${FILTERS[*]}" >&2
  echo "verify: available checks:" >&2
  all_checks | sed 's/^/  /' >&2
  exit 1
fi

if [[ ! -x $BINARY ]]; then
  echo "verify: '$BINARY' is not executable" >&2
  exit 1
fi
BINARY=$(realpath "$BINARY")
BINARY_DIR=$(dirname "$BINARY")

# Checks use helper clients built alongside the selected compositor. Keeping
# this resolution here makes every build mode consistent without each recipe
# having to export a matching set of paths.
export UMBRIEL_POINTER_CLIENT="$BINARY_DIR/pointer-client"
export UMBRIEL_INPUT_METHOD_CLIENT="$BINARY_DIR/input-method-client"
export UMBRIEL_DRAG_CLIENT="$BINARY_DIR/drag-client"
export UMBRIEL_GLOBAL_CLIENT="$BINARY_DIR/global-client"
export UMBRIEL_WORKSPACE_CLIENT="$BINARY_DIR/workspace-client"
export UMBRIEL_UNMAP_CLIENT="$BINARY_DIR/unmap-client"
export UMBRIEL_IDLE_INHIBIT_CLIENT="$BINARY_DIR/idle-inhibit-client"

# sockaddr_un caps paths at 108 bytes and the compositor appends "/umbriel-wayland-0.sock" (23) to XDG_RUNTIME_DIR, so keep the root short. A long path makes
# wl_display_add_socket fail and the boot abort.
RUNTIME_DIR=$(mktemp -d /tmp/umv.XXXXXXXX)
LOG=$RUNTIME_DIR/compositor.log
CONFIG=$RUNTIME_DIR/config.toml
SERVER_PID=
IPC_CLIENT_PID=
KEEP_RUNTIME_DIR=0

cleanup() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -n $IPC_CLIENT_PID ]] && kill -0 "$IPC_CLIENT_PID" 2>/dev/null; then
    kill -KILL "$IPC_CLIENT_PID" 2>/dev/null || true
    wait "$IPC_CLIENT_PID" 2>/dev/null || true
  fi
  # A failed run is worth debugging, and its evidence (compositor log, per-client
  # logs, config) lives here. Keep it and let the summary point at it.
  if ((KEEP_RUNTIME_DIR)); then
    return
  fi
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

now_us() {
  # EPOCHREALTIME is "seconds.microseconds" with a locale-dependent radix, so
  # dropping the separator yields plain microseconds without spawning a process.
  local stamp=${EPOCHREALTIME:-}
  if [[ -z $stamp ]]; then
    date +%s%6N
    return
  fi
  echo "${stamp/[.,]/}"
}

elapsed() {
  local us=$(($(now_us) - $1))
  printf '%d.%02ds' "$((us / 1000000))" "$((us % 1000000 / 10000))"
}

header() {
  printf '%s\n' "${C_BOLD}verify${C_OFF} ${C_DIM}·${C_OFF} $BINARY"
  printf '%s\n' "       ${C_DIM}·${C_OFF} runtime $RUNTIME_DIR"
  local scope="${#SELECTED[@]} of $TOTAL checks"
  ((${#FILTERS[@]} > 0)) && scope+=" (filter: ${FILTERS[*]})"
  printf '%s\n\n' "       ${C_DIM}·${C_OFF} $scope"
}

start_row() {
  ((TTY)) || return 0
  printf '  %sRUN %s %s%s%s' "$C_RUN" "$C_OFF" "$C_DIM" "$1" "$C_OFF"
}

# Pass detail is context, not a finding: one dimmed line, elided to the terminal
# width. Failure detail is the finding itself and is never trimmed.
detail() {
  local status=$1 text=$2
  [[ -z $text ]] && return 0
  if [[ $status == PASS ]] && ((!VERBOSE)); then
    local first=${text%%$'\n'*}
    local room=$((COLUMNS_MAX - 7))
    if [[ ${#first} -gt $room || $first != "$text" ]]; then
      first=${first:0:room}…
    fi
    printf '%s\n' "       ${C_DIM}${first}${C_OFF}"
    return 0
  fi
  local marker="${C_DIM}│${C_OFF}"
  [[ $status == FAIL ]] && marker="${C_FAIL}│${C_OFF}"
  while IFS= read -r line; do
    printf '%s\n' "     $marker $line"
  done <<< "$text"
}

row() {
  local status=$1 name=$2 duration=$3 text=${4:-}
  local colour=$C_PASS
  [[ $status == FAIL ]] && colour="${C_FAIL}${C_BOLD}"
  ((TTY)) && printf '\r\e[2K'
  printf '  %s%s%s %-*s %s%7s%s\n' "$colour" "$status" "$C_OFF" "$NAME_WIDTH" "$name" "$C_DIM" "$duration" "$C_OFF"
  detail "$status" "$text"
}

# No autostart, no xwayland, no cheatsheet: the harness wants a bare compositor,
# and each of those would spawn processes outside the container.
cat > "$CONFIG" << 'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF

header
suite_start=$(now_us)
boot_start=$suite_start
start_row boot
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
    KEEP_RUNTIME_DIR=1
    row FAIL boot "$(elapsed "$boot_start")" "compositor died during boot"$'\n'"$(< "$LOG")"
    exit 1
  fi
  sleep 0.25
done
if [[ ! -S $UMBRIEL_SOCKET ]]; then
  KEEP_RUNTIME_DIR=1
  row FAIL boot "$(elapsed "$boot_start")" "IPC socket never appeared"$'\n'"$(< "$LOG")"
  exit 1
fi
row PASS boot "$(elapsed "$boot_start")" "headless compositor answering IPC on $UMBRIEL_SOCKET"

export UMBRIEL=$BINARY
export UMBRIEL_RUNTIME_DIR=$RUNTIME_DIR
export UMBRIEL_LOG=$LOG
export UMBRIEL_CONFIG=$CONFIG

passed=0
FAILED_NAMES=()
for name in "${SELECTED[@]}"; do
  start_row "$name"
  check_start=$(now_us)
  if output=$(bash "$HARNESS_DIR/checks/$name.sh" 2>&1); then
    row PASS "$name" "$(elapsed "$check_start")" "$output"
    passed=$((passed + 1))
  else
    row FAIL "$name" "$(elapsed "$check_start")" "$output"
    FAILED_NAMES+=("$name")
  fi
done

# Clean shutdown is itself a check: a listener still attached to a wlroots object at teardown trips an assert and the process dies on SIGABRT (exit 134) after having already logged "shutting down". Keep one incomplete IPC connection registered through teardown. Completed connections were exercised by the checks above; both lifecycle paths must leave no event source or descriptor behind.
start_row shutdown
shutdown_start=$(now_us)
IPC_READY=$RUNTIME_DIR/ipc-idle-ready
python3 - "$UMBRIEL_SOCKET" "$IPC_READY" << 'PY' &
import pathlib
import socket
import sys
import time

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(sys.argv[1])
client.sendall(b"{")
pathlib.Path(sys.argv[2]).touch()
time.sleep(30)
PY
IPC_CLIENT_PID=$!
for _ in $(seq 20); do
  [[ -f $IPC_READY ]] && break
  sleep 0.01
done
setup_failed=0
if [[ ! -f $IPC_READY ]]; then
  setup_failed=1
fi

kill -TERM "$SERVER_PID"
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=
if [[ -n $IPC_CLIENT_PID ]] && kill -0 "$IPC_CLIENT_PID" 2>/dev/null; then
  kill -KILL "$IPC_CLIENT_PID" 2>/dev/null || true
  wait "$IPC_CLIENT_PID" 2>/dev/null || true
fi
IPC_CLIENT_PID=

if ((setup_failed)); then
  row FAIL shutdown "$(elapsed "$shutdown_start")" "idle IPC client never connected, so teardown ran without one"
  FAILED_NAMES+=(shutdown)
elif [[ $status -ne 0 ]]; then
  row FAIL shutdown "$(elapsed "$shutdown_start")" "exit status $status, expected 0"$'\n'"$(tail -5 "$LOG")"
  FAILED_NAMES+=(shutdown)
else
  row PASS shutdown "$(elapsed "$shutdown_start")" "terminated cleanly with an incomplete IPC connection attached"
fi

failed=${#FAILED_NAMES[@]}
total_time=$(elapsed "$suite_start")
printf '\n'
if ((failed > 0)); then
  KEEP_RUNTIME_DIR=1
  printf '%s\n' "  ${C_FAIL}${C_BOLD}${failed} failed${C_OFF} ${C_DIM}·${C_OFF} $passed passed ${C_DIM}·${C_OFF} ${C_DIM}${total_time}${C_OFF}"
  for name in "${FAILED_NAMES[@]}"; do
    printf '%s\n' "    ${C_FAIL}·${C_OFF} $name"
  done
  printf '%s\n' "  ${C_DIM}logs kept in $RUNTIME_DIR (compositor.log and per-client logs)${C_OFF}"
  exit 1
fi
printf '%s\n' "  ${C_PASS}${C_BOLD}${passed} passed${C_OFF} ${C_DIM}·${C_OFF} ${C_DIM}${total_time}${C_OFF}"
