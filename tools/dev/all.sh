# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `all` target: runs every lane's build/check.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_lanes() { echo firmware simulator host macos protocol; }

_for_each_lane() {
  local command="$1"
  local lane script failed=0
  for lane in $(_lanes); do
    script="$(dirname "${BASH_SOURCE[0]}")/$lane.sh"
    echo "=== $command $lane ==="
    (
      # shellcheck source=/dev/null
      source "$script"
      "$command"
    ) || {
      echo "!!! $command $lane failed" >&2
      failed=1
    }
  done
  return $failed
}

doctor() {
  _for_each_lane doctor
}

build() {
  _for_each_lane build
}

check() {
  _for_each_lane check
}

run() {
  echo "error: 'run' has no meaning for the 'all' target; run a specific target" >&2
  return 1
}
