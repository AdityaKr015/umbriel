#!/usr/bin/env bash
# Unchanged committed color must deliver requested callbacks without pixel damage.
# Real color transitions must repaint, including cached and independent child commits.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.blur]
enabled = false
[output.HEADLESS-1]
hdr = "off"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/color-damage-capture.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/color-damage-capture.fifo"
CAPTURE_CONTROL=1 "$UMBRIEL_CAPTURE_CLIENT" < "$UMBRIEL_RUNTIME_DIR/color-damage-capture.fifo" > "$CAPTURE_LOG" 2>&1 &
exec {CAPTURE_FD}> "$UMBRIEL_RUNTIME_DIR/color-damage-capture.fifo"
await_lines "$CAPTURE_LOG" 'session-ready HEADLESS-1' 1
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1

barrier=0
capture_count() {
  barrier=$((barrier + 1))
  printf 'sync color-%s\n' "$barrier" >&"$CAPTURE_FD"
  await_lines "$CAPTURE_LOG" "synced color-$barrier HEADLESS-1 " 1
  frame_count=$(awk -v label="color-$barrier" '$1 == "synced" && $2 == label && $3 == "HEADLESS-1" { print $4 }' "$CAPTURE_LOG")
  [[ $frame_count =~ ^[0-9]+$ ]]
}

request() {
  local command=$1 previous
  previous=$(events "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"" "$((previous + 1))"
}

expect_frames() {
  local command=$1 expected=$2 description=$3 before after callbacks
  capture_count
  before=$frame_count
  callbacks=$(events "$CLIENT_LOG" '{"event":"frame",')
  request "$command"
  if [[ $command == frame ]]; then
    await_lines "$CLIENT_LOG" '{"event":"frame",' "$((callbacks + 1))"
  fi
  "$UMBRIEL" settle > /dev/null
  capture_count
  after=$frame_count
  if ((after - before != expected)); then
    echo "$mode: $description changed capture count $before to $after, expected $expected drawn frame(s)"
    exit 1
  fi
}

for mode in toplevel subsurface subsurface-desync; do
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/color-damage-$mode.log"
  fifo="$UMBRIEL_RUNTIME_DIR/color-damage-$mode.fifo"
  mkfifo "$fifo"
  args=()
  [[ $mode == toplevel ]] || args+=("$mode")
  "$UMBRIEL_COLOR_FEEDBACK_CLIENT" "${args[@]}" < "$fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
  "$UMBRIEL" settle > /dev/null

  expect_frames frame 0 'a PQ callback-only commit'
  expect_frames commit 0 'an unchanged PQ surface commit'
  expect_frames tag 0 'the same PQ description'
  expect_frames unset 1 'the PQ to sRGB transition'
  expect_frames frame 0 'an sRGB callback-only commit'
  expect_frames unset 0 'the same unset description'
  expect_frames tag 1 'the sRGB to PQ transition'
  expect_frames frame 0 'a callback after restoring PQ'

  if [[ $mode == subsurface ]]; then
    expect_frames cache 0 'a synchronized cached color change'
    expect_frames parent 1 'the cached sRGB state applied by its parent'
    expect_frames apply 1 'the newer pending PQ state applied by its child'
    expect_frames frame 0 'a callback after synchronized state application'
  fi

  expect_frames destroy-only 1 'color-object destruction without a surface commit'
  expect_frames frame 0 'a callback after color-object destruction'
  expect_frames recreate 1 'recreating the color object with PQ'
  expect_frames frame 0 'a callback after color-object recreation'
  echo "$mode: unchanged colors stayed quiet, callbacks completed, actual color changes repainted"

  kill -TERM "$client_pid"
  wait "$client_pid" || true
  exec {CLIENT_FD}>&-
  "$UMBRIEL" settle > /dev/null
done
