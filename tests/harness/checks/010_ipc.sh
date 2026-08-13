#!/usr/bin/env bash
# The IPC surface answers and returns well-formed JSON of the documented shape.
set -euo pipefail

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
