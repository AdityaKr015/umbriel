#!/usr/bin/env bash
# Workspace selectors resolve, and bad ones report a useful error. The selector logic is the densest branch in the action dispatch: qualified `<workspace>/<output>` addresses one group, an unqualified numeric selector prefers the focused output's dynamic group, and an unqualified name falls back to a unique match anywhere before failing. The failure paths carry the messages users actually see, so they are asserted by text, not just by status.
set -euo pipefail

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

rejects_with() {
  local action=$1 expected=$2
  if out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "expected '$action' to be rejected, but it succeeded"
    return 1
  fi
  if [[ $out != *"$expected"* ]]; then
    echo "expected '$action' to mention '$expected', got: $out"
    return 1
  fi
}

# Unqualified, and qualified to the one output the harness has.
accepts "workspace-switch:1"
accepts "workspace-switch:1/HEADLESS-1"
accepts "window-move-to-workspace:1"

# Dynamic workspaces are created on demand, so any number resolves.
accepts "workspace-switch:2"
accepts "workspace-switch:99"
accepts "workspace-switch:1"

rejects_with "workspace-switch:1/NOPE" "unknown output: NOPE"
rejects_with "workspace-switch:nosuchname" "unknown workspace: nosuchname"
rejects_with "window-move-to-workspace:1/NOPE" "unknown output: NOPE"

echo "selectors resolve; unknown output and unknown workspace both reported"
