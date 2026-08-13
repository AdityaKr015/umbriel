#!/usr/bin/env bash
# The IPC surface answers and returns well-formed JSON of the documented shape.
set -euo pipefail

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
PY

windows=$("$UMBRIEL" windows --json)
if ! jq -e 'type == "array"' <<< "$windows" > /dev/null; then
  echo "windows --json is not an array: $windows"
  exit 1
fi

layers=$("$UMBRIEL" layers --json)
if ! jq -e 'type == "array"' <<< "$layers" > /dev/null; then
  echo "layers --json is not an array: $layers"
  exit 1
fi

# An unknown action must be rejected, not silently accepted.
if "$UMBRIEL" msg definitely-not-an-action > /dev/null 2>&1; then
  echo "msg accepted an unknown action"
  exit 1
fi
