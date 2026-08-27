#!/usr/bin/env bash
# The IPC surface answers and returns well-formed JSON of the documented shape.
set -euo pipefail

spawn_client() {
  foot --title="ipc-client" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want window(s), got $("$UMBRIEL" windows --json | jq 'length'): $("$UMBRIEL" windows --json)"
  return 1
}

python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" <<'PY'
import json
import socket
import subprocess
import sys
import time

socket_path, umbriel = sys.argv[1:]


def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(socket_path)
    return client


def request(payload):
    client = connect()
    client.sendall(payload)
    response = b""
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        response += chunk
    client.close()
    return json.loads(response)


stalled = connect()
stalled.sendall(b"{")
time.sleep(0.05)
started = time.monotonic()
probe = subprocess.run(
    [umbriel, "windows", "--json"],
    check=False,
    capture_output=True,
    text=True,
    timeout=2,
)
elapsed = time.monotonic() - started
if probe.returncode != 0:
    raise SystemExit(f"parallel IPC request failed: {probe.stderr.strip()}")
if elapsed >= 0.3:
    raise SystemExit(f"partial client delayed parallel IPC by {elapsed * 1000:.1f} ms")
if not isinstance(json.loads(probe.stdout), list):
    raise SystemExit("parallel windows response is not an array")

stalled.settimeout(2)
try:
    while stalled.recv(4096):
        pass
except socket.timeout:
    raise SystemExit("partial client exceeded the connection deadline")
finally:
    stalled.close()

if request(b"{}\n").get("err") != "malformed request":
    raise SystemExit("malformed request did not return its protocol error")
if request(b"x" * 65537).get("err") != "request too long":
    raise SystemExit("oversized request did not return its protocol error")

two = request(b'{"cmd":"windows"}\n{"cmd":"layers"}\n')
if "ok" not in two or not isinstance(two["ok"], list):
    raise SystemExit("one-request connection returned a malformed response")

# Unknown window ids must be rejected by both window actions with the same
# error, delivered through the msg action path.
for action in ("window-focus", "window-close"):
    reply = request(f'{{"cmd":"msg","arg":"{action}:definitely-not-a-window"}}\n'.encode())
    if reply.get("err") != "unknown window: definitely-not-a-window":
        raise SystemExit(f"{action} did not reject an unknown window id: {reply}")
PY

spawn_client
wait_for_windows 1

windows=$("$UMBRIEL" windows --json)
if ! jq -e 'type == "array"' <<< "$windows" > /dev/null; then
  echo "windows --json is not an array: $windows"
  exit 1
fi
# Every window entry carries the identity fields noctalia joins on, with the workspace id in the ext-workspace "<output>:<serial>" shape and a boolean
# seat-global active flag distinct from the per-workspace focused flag.
if ! jq -e '
  all(.[];
    (has("id") and (.id | type == "string"))
    and (has("workspace") and (.workspace | type == "string"))
    and (has("active") and (.active | type == "boolean"))
  )' <<< "$windows" > /dev/null; then
  echo "windows entries lack id/workspace/active: $windows"
  exit 1
fi
if ! jq -e '.[0].id != "" and (.[0].workspace | test("^HEADLESS-1:[0-9]+$"))' <<< "$windows" > /dev/null; then
  echo "windows id/workspace have an unexpected shape: $windows"
  exit 1
fi
if [[ $(jq -r '.[0].active' <<< "$windows") != $(jq -r '.[0].focused' <<< "$windows") ]]; then
  echo "active flag does not match the focused state of the only window: $windows"
  exit 1
fi

workspaces=$("$UMBRIEL" workspaces --json)
if ! jq -e '
  type == "array" and length >= 1
  and all(.[];
    (has("id") and (.id | type == "string"))
    and (has("name") and (.name | type == "string"))
    and (has("index") and (.index | type == "number") and .index >= 1)
    and (has("output") and (.output | type == "string"))
    and (has("active") and (.active | type == "boolean"))
    and (has("focused") and (.focused | type == "boolean"))
    and (has("layout") and (.layout | type == "string"))
  )
  and ([.[] | select(.active)] | length == 1)
  and ([.[] | select(.focused)] | length == 1)
  and all(.[] | select(.focused); .active)
  and all(.[]; .layout == "scrolling")
' <<< "$workspaces" > /dev/null; then
  echo "workspaces --json has an unexpected initial shape: $workspaces"
  exit 1
fi
if ! jq -e '
  .[0].index == 1
  and .[0].name == "1"
  and .[0].output == "HEADLESS-1"
  and (.[0].id | test("^HEADLESS-1:[0-9]+$"))
' <<< "$workspaces" > /dev/null; then
  echo "first workspace has unexpected identity fields: $workspaces"
  exit 1
fi

# The listing reports the effective mode, including a runtime override, rather
# than only the mode last loaded from configuration.
"$UMBRIEL" msg workspace-set-layout:dwindle > /dev/null
workspaces=$("$UMBRIEL" workspaces --json)
if ! jq -e '[.[] | select(.focused) | .layout] == ["dwindle"]' <<< "$workspaces" > /dev/null; then
  echo "focused workspace did not report its runtime layout override: $workspaces"
  exit 1
fi
workspace_human=$("$UMBRIEL" workspaces)
if ! grep -F "* HEADLESS-1: 1 [dwindle] (focused)" <<< "$workspace_human" > /dev/null; then
  echo "human workspace listing does not identify the focused layout: $workspace_human"
  exit 1
fi
"$UMBRIEL" msg workspace-set-layout:scrolling > /dev/null

submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == null' <<< "$submap" > /dev/null; then
  echo "submap --json did not report the default context: $submap"
  exit 1
fi
if [[ $("$UMBRIEL" submap | wc -c) -ne 0 ]]; then
  echo "human submap output printed text for the default context"
  exit 1
fi

# Submaps nest, so current means the top layer that handles keybinds. Popping
# that layer must reveal the previous one before returning to the default map.
"$UMBRIEL" msg submap:outer > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == "outer"' <<< "$submap" > /dev/null; then
  echo "submap --json did not report the active outer layer: $submap"
  exit 1
fi
if [[ $("$UMBRIEL" submap) != "outer" ]]; then
  echo "human submap output did not report the active outer layer"
  exit 1
fi
"$UMBRIEL" msg submap:inner > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == "inner"' <<< "$submap" > /dev/null; then
  echo "submap --json did not report the nested top layer: $submap"
  exit 1
fi
"$UMBRIEL" msg submap:reset > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == "outer"' <<< "$submap" > /dev/null; then
  echo "submap reset did not reveal the previous layer: $submap"
  exit 1
fi
"$UMBRIEL" msg submap:reset > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == null' <<< "$submap" > /dev/null; then
  echo "submap reset did not restore the default context: $submap"
  exit 1
fi

layers=$("$UMBRIEL" layers --json)
if ! jq -e 'type == "array"' <<< "$layers" > /dev/null; then
  echo "layers --json is not an array: $layers"
  exit 1
fi

# The headless harness may have no physical keyboard, in which case the command
# errors with "no keyboard". Exactly one of the two shapes must appear.
if layouts=$("$UMBRIEL" keyboard-layouts --json 2>/dev/null); then
  if ! jq -e '
    type == "object"
    and (has("names") and (.names | type == "array"))
    and (has("current_index") and (.current_index | type == "number"))
  ' <<< "$layouts" > /dev/null; then
    echo "keyboard-layouts --json has an unexpected shape: $layouts"
    exit 1
  fi
else
  err=$("$UMBRIEL" keyboard-layouts 2>&1 >/dev/null) || true
  if [[ $err != *"no keyboard"* ]]; then
    echo "keyboard-layouts neither returned layouts nor the no-keyboard error"
    exit 1
  fi
fi

# An unknown action must be rejected, not silently accepted.
if "$UMBRIEL" msg definitely-not-an-action > /dev/null 2>&1; then
  echo "msg accepted an unknown action"
  exit 1
fi

python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" <<'PY'
import json
import socket
import subprocess
import sys
import time

socket_path, umbriel = sys.argv[1:]


def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(socket_path)
    return client


def request(payload):
    client = connect()
    client.sendall(payload)
    response = b""
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        response += chunk
    client.close()
    return json.loads(response)


def read_one(client, buf):
    client.settimeout(5)
    while b"\n" not in buf:
        chunk = client.recv(4096)
        if not chunk:
            return None, buf
        buf += chunk
    line, buf = buf.split(b"\n", 1)
    return line, buf


# The subscribed connection stays open and first delivers the initial state of
# every subscribed event, in the fixed order overview, windows.
sub = connect()
sub.sendall(b'{"cmd":"subscribe","events":["overview","windows"]}\n')
buf = b""
initial = []
for _ in range(2):
    line, buf = read_one(sub, buf)
    if line is None:
        break
    initial.append(line)
if len(initial) != 2:
    raise SystemExit(f"subscribe returned {len(initial)} initial line(s), expected 2: {initial!r}")

first = json.loads(initial[0])
second = json.loads(initial[1])
if first.get("event") != "overview" or not isinstance(first.get("data", {}).get("open"), bool):
    raise SystemExit(f"first initial event is not an overview event: {initial[0]}")
if second.get("event") != "windows" or not isinstance(second.get("data"), list):
    raise SystemExit(f"second initial event is not a windows event: {initial[1]}")
if not second["data"]:
    raise SystemExit("windows initial event has no windows")
for key in ("id", "workspace", "active"):
    if key not in second["data"][0]:
        raise SystemExit(f"windows event entry lacks '{key}': {second['data'][0]}")
real_id = second["data"][0]["id"]
if not real_id:
    raise SystemExit("windows event entry has an empty id")

# The focus round trip resolves the exact identifier; the same lookup must
# also close the window.
focused = request(f'{{"cmd":"msg","arg":"window-focus:{real_id}"}}\n'.encode())
if "err" in focused or "ok" not in focused:
    raise SystemExit(f"window-focus on the real window failed: {focused}")

# Other actions can push windows events (focus changes schedule them), so skip
# every line until the pushed overview event arrives.
subprocess.run([umbriel, "msg", "overview-open"], check=True, capture_output=True, text=True, timeout=5)
event = None
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    line, buf = read_one(sub, buf)
    if line is None:
        break
    parsed = json.loads(line)
    if parsed.get("event") == "overview":
        event = parsed
        break
if event is None or event.get("data", {}).get("open") is not True:
    raise SystemExit(f"overview-open did not push an open event: {event!r}")
sub.close()
subprocess.run([umbriel, "msg", "overview-close"], check=True, capture_output=True, text=True, timeout=5)

closed = request(f'{{"cmd":"msg","arg":"window-close:{real_id}"}}\n'.encode())
if "err" in closed or "ok" not in closed:
    raise SystemExit(f"window-close on the real window failed: {closed}")
PY

# An "ok" reply only says the request was accepted. The window must actually
# leave the list, which is the close path itself, not tidying up after it.
wait_for_windows 0
