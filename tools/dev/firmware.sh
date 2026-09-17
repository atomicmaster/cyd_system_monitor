# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `firmware` target (ESP-IDF + E32R28T profile).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_firmware_dir() { echo "$(dev_root)/firmware"; }

doctor() {
  report_tool idf.py "$(idf_missing_hint)"
  report_tool cmake "brew install cmake"
  report_tool ninja "brew install ninja"
  report_tool python3 "install Python 3 (used by profile-to-header generation)"
  report_tool clang-format "brew install clang-format (optional at M0)"
}

_check_format() {
  if command -v clang-format >/dev/null 2>&1; then
    # Exclude build/ and build-<name>/ (generated; the latter are the
    # reversible capacity-probe trees built with idf.py -B) and
    # managed_components/ (vendored third-party sources fetched by the
    # ESP-IDF component manager, e.g. lvgl/lvgl and esp_lcd_ili9341) --
    # this project doesn't own their formatting.
    find "$(_firmware_dir)" \( -name '*.hpp' -o -name '*.cpp' \) \
      -not -path '*/build/*' -not -path '*/build-*/*' \
      -not -path '*/managed_components/*' \
      -print0 | xargs -0 clang-format --dry-run --Werror
  else
    echo "note: clang-format not installed; skipping format check (see ./dev doctor)"
  fi
}

build() {
  require_tool idf.py "$(idf_missing_hint)" || return 1
  (cd "$(_firmware_dir)" && idf.py build && idf.py size)
}

# Builds and runs firmware/domain/tests with the host compiler (no ESP-IDF
# required), proving the domain target is usable independently of ESP-IDF.
check() {
  require_tool cmake "brew install cmake" || return 1
  require_tool ninja "brew install ninja" || return 1
  require_tool python3 "install Python 3" || return 1
  _check_format
  python3 "$(_firmware_dir)/tools/test_generate_profile_header.py"
  local test_dir="$(_firmware_dir)/domain/tests"
  cmake -S "$test_dir" -B "$test_dir/build" -G Ninja
  cmake --build "$test_dir/build"
  "$test_dir/build/firmware_domain_tests"
  "$test_dir/build/firmware_domain_calibration_tests"
  "$test_dir/build/firmware_domain_diagnostic_tests"
  "$test_dir/build/firmware_domain_feasibility_tests"
  "$test_dir/build/firmware_domain_capacity_tests"
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  require_tool idf.py "$(idf_missing_hint)" || return 1
  (cd "$(_firmware_dir)" && idf.py "$@")
}
