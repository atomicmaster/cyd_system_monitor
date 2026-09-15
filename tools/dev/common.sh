# SPDX-License-Identifier: GPL-3.0-only
# Shared helpers for tools/dev/*.sh. Sourced, not executed directly.

dev_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

require_tool() {
  local tool="$1" hint="$2"
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required tool '$tool' not found. $hint" >&2
    return 1
  fi
}

report_tool() {
  local tool="$1" hint="$2"
  if command -v "$tool" >/dev/null 2>&1; then
    echo "  [ok] $tool: $(command -v "$tool")"
  else
    echo "  [missing] $tool -- $hint"
  fi
}
