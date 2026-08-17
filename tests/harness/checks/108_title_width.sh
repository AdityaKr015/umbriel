#!/usr/bin/env bash
# A title change after map must not reapply an unchanged default_width rule.
set -euo pipefail

readonly CONFIG_BACKUP="$UMBRIEL_CONFIG.title-width.bak"
readonly TITLE_FIFO="$UMBRIEL_RUNTIME_DIR/title-width.fifo"
CLIENT_PID=

cleanup() {
  if [[ -f $CONFIG_BACKUP ]]; then
    cp "$CONFIG_BACKUP" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null 2>&1 || true
  fi
  if [[ -n $CLIENT_PID ]]; then
    kill -KILL "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
  fi
  rm -f "$TITLE_FIFO"
}
trap cleanup EXIT

window_width() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "helium") | .w'
}

wait_for_title() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "helium") | .title') == helium-navigated ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for helium title change: $("$UMBRIEL" windows --json)"
  return 1
}

cp "$UMBRIEL_CONFIG" "$CONFIG_BACKUP"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^helium$"
default_width = 0.75

[[window_rule]]
match.title = "^unrelated-title-rule$"
opacity = 0.9
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$TITLE_FIFO"
env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$UMBRIEL_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 TITLE_FIFO="$TITLE_FIFO" \
  foot --app-id=helium --title=helium-initial sh -c \
    'IFS= read -r title < "$TITLE_FIFO"; printf "\033]2;%s\007" "$title"; sleep 120' \
    > /dev/null 2>&1 &
CLIENT_PID=$!

for _ in $(seq 60); do
  [[ $(window_width) == 942 ]] && break
  sleep 0.1
done
initial_width=$(window_width)
if [[ $initial_width != 942 ]]; then
  echo "default_width did not produce the expected initial width: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg window-set-width:0.25 > /dev/null
sleep 0.3
manual_width=$(window_width)
if (( manual_width >= initial_width )); then
  echo "window-set-width did not shrink helium: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo helium-navigated > "$TITLE_FIFO"
wait_for_title
final_width=$(window_width)
if [[ $final_width != "$manual_width" ]]; then
  echo "title change reset manual width from $manual_width to $final_width: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "title change retained the manually selected width"
