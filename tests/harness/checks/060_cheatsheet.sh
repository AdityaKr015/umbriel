#!/usr/bin/env bash
# The cheatsheet renders without taking the compositor down.
#
# Its render path walks every configured keybind, builds a label per bind from
# the action spec and the bind's payload, then lays the result out with pango.
# None of that is observable over IPC, so this cannot assert what is drawn. What
# it can assert is that building and tearing down the overlay repeatedly leaves
# a live, responsive compositor, which is what a bad label lookup or a null
# payload would break.
set -euo pipefail

alive() {
  if ! "$UMBRIEL" windows --json > /dev/null 2>&1; then
    echo "compositor stopped answering after $1"
    return 1
  fi
}

"$UMBRIEL" msg cheatsheet-open > /dev/null
alive "cheatsheet-open"

"$UMBRIEL" msg cheatsheet-close > /dev/null
alive "cheatsheet-close"

# Toggling repeatedly exercises the destroy-and-rebuild path, which is how the
# overlay is re-rendered on every relayout.
for _ in 1 2 3; do
  "$UMBRIEL" msg cheatsheet-toggle > /dev/null
done
alive "cheatsheet-toggle"

"$UMBRIEL" msg cheatsheet-close > /dev/null
alive "final close"

echo "open, close, and repeated toggle all survive"
