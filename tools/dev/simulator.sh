# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `simulator` target (desktop LVGL build).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_simulator_dir() { echo "$(dev_root)/simulator"; }
_simulator_build_dir() { echo "$(_simulator_dir)/build"; }

doctor() {
  report_tool cmake "brew install cmake"
  report_tool ninja "brew install ninja"
  report_tool python3 "install Python 3 (used by profile-to-header generation)"
  report_tool clang-format "brew install clang-format (optional at M0)"
}

_check_format() {
  if command -v clang-format >/dev/null 2>&1; then
    find "$(_simulator_dir)" \( -name '*.hpp' -o -name '*.cpp' \) -not -path '*/build/*' \
      -print0 | xargs -0 clang-format --dry-run --Werror
  else
    echo "note: clang-format not installed; skipping format check (see ./dev doctor)"
  fi
}

_configure() {
  require_tool cmake "brew install cmake" || return 1
  require_tool ninja "brew install ninja" || return 1
  cmake -S "$(_simulator_dir)" -B "$(_simulator_build_dir)" -G Ninja
}

build() {
  _configure || return 1
  cmake --build "$(_simulator_build_dir)"
}

check() {
  _check_format
  build
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  build || return 1
  "$(_simulator_build_dir)/simulator" "$@"
}
