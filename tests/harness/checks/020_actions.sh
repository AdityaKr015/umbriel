#!/usr/bin/env bash
# Every action `umbriel actions` advertises is parseable and accepted by `msg`.
#
# This is the regression net for the action registry: the roadmap's R7 collapses
# the enum, the spec table, and the dispatch switch into one table, and R8
# reshapes the payload. Both are exactly the kind of change that can silently
# drop an action, and the compiler will not notice.
set -euo pipefail

# Actions deliberately not exercised: they act on the harness itself rather than
# on compositor state.
skip_action() {
  case $1 in
    session-quit) return 0 ;;  # would kill the instance mid-run
    spawn)        return 0 ;;  # would start a process outside the container
    *)            return 1 ;;
  esac
}

# Sample argument per parameterized action, keyed by the spec's param text.
sample_arg() {
  case $1 in
    '<cmd>')                     echo 'true' ;;
    '<fraction>')                echo '0.5' ;;
    '<workspace>[/<output>]')    echo '1' ;;
    '<name>')                    echo 'harness' ;;
    '[<output>]')                echo '' ;;
    *)                           echo '' ;;
  esac
}

failures=0
count=0
while read -r spec; do
  [[ -z $spec ]] && continue
  name=${spec%%:*}
  param=''
  [[ $spec == *:* ]] && param=${spec#*:}

  skip_action "$name" && continue

  arg=$(sample_arg "$param")
  action=$name
  [[ -n $arg ]] && action="$name:$arg"

  count=$((count + 1))
  if ! out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "rejected: $action -> $out"
    failures=$((failures + 1))
  fi
done < <("$UMBRIEL" actions)

if [[ $count -eq 0 ]]; then
  echo "no actions were exercised"
  exit 1
fi

# A submap push leaves the compositor in that submap; pop it so later checks
# see the default keybind set.
"$UMBRIEL" msg submap:reset > /dev/null 2>&1 || true

if [[ $failures -gt 0 ]]; then
  echo "$failures of $count actions failed"
  exit 1
fi
echo "$count actions accepted"
