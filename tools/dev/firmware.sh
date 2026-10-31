# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `firmware` target (ESP-IDF + E32R28T profile).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_firmware_dir() { echo "$(dev_root)/firmware"; }

doctor() {
  python3 "$(dev_root)/tools/dev/sdkconfig_drift.py"
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
  if [[ "${1:-}" == "--" ]]; then shift; fi
  if [[ "${1:-}" == "--fresh" ]]; then
    # A fresh build regenerates its own sdkconfig from the defaults, so the
    # local firmware/sdkconfig (and any drift in it) does not apply.
    local fresh="$(_firmware_dir)/build-fresh"
    rm -f "$fresh/sdkconfig"
    (cd "$(_firmware_dir)" && idf.py -B "$fresh" -D "SDKCONFIG=$fresh/sdkconfig" build && idf.py -B "$fresh" -D "SDKCONFIG=$fresh/sdkconfig" size)
  else
    python3 "$(dev_root)/tools/dev/sdkconfig_drift.py"
    (cd "$(_firmware_dir)" && idf.py build && idf.py size)
  fi
}

# Builds and runs firmware/domain/tests with the host compiler (no ESP-IDF
# required), proving the domain target is usable independently of ESP-IDF.
check() {
  require_tool cmake "brew install cmake" || return 1
  require_tool ninja "brew install ninja" || return 1
  require_tool python3 "install Python 3" || return 1
  python3 "$(dev_root)/tools/dev/tests/test_sdkconfig_drift.py"
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
  "$test_dir/build/firmware_domain_host_state_tests"
  "$test_dir/build/firmware_domain_host_link_tests"
  "$test_dir/build/firmware_domain_display_settings_tests"
  "$test_dir/build/firmware_domain_coverage_tests"
  "$test_dir/build/firmware_domain_event_ring_tests"
  "$test_dir/build/firmware_domain_radio_runner_tests"
  "$test_dir/build/firmware_domain_hci_tests"
  run_parity_gate firmware
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  require_tool idf.py "$(idf_missing_hint)" || return 1
  (cd "$(_firmware_dir)" && idf.py "$@")
}
