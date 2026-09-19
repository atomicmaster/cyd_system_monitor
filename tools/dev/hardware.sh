# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `hardware` target: an explicit board runner for
# the connected E32R28T session. build/check delegate to firmware.sh's
# functions rather than duplicating them -- only `run` (which requires a
# physically connected board and a confirmed serial port) is unique here.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_firmware_script() { echo "$(dirname "${BASH_SOURCE[0]}")/firmware.sh"; }

doctor() {
  report_tool idf.py "$(idf_missing_hint)"
  echo "  [note] 'run' additionally requires a connected E32R28T board and a confirmed serial port (--port)."
  echo "  [note] flashing, port use, and diagnostic registration are serialized across F05/F06/F07; see docs/tickets/README.md#dispatch-and-completion."
}

# Delegates to firmware.sh's build: `idf.py build && idf.py size`. No board
# access is needed to build a flashable image.
build() {
  # shellcheck source=/dev/null
  source "$(_firmware_script)"
  build
}

# Delegates to firmware.sh's check: the native CMake/Ninja domain build and
# test run, which needs no ESP-IDF install and no board.
check() {
  # shellcheck source=/dev/null
  source "$(_firmware_script)"
  check
}

# Runs a diagnostic case on a connected board:
#   ./dev run hardware -- <case> --port <path>
#     case: display-smoke | calibration | peripherals | capacity | cpu-replay
#
# Unlike the simulator (which builds a separate scenario binary per case
# because it has no operator to drive it interactively), the real board
# runs a single always-on diagnostic image: firmware/main/main.cpp boots
# straight into the diagnostic screen, runs guided calibration
# automatically on an uncalibrated touch panel, and continuously reports
# every peripheral's status. There is no firmware-side case switch to
# plumb. `<case>` selects which ticket's acceptance bullets the operator
# is expected to exercise and record evidence for during this session
# (display-smoke: F05: readable landscape diagnostics, reset/build
# identity over UART; calibration: F06: guided 5-point calibration,
# validation, persistence across reset, USB recovery command; peripherals:
# F07: LED/audio/SD/battery/button rows; capacity: F08a: send
# DEV:CAPACITY_STATUS over the dev console while exercising touch, display
# refresh, and representative serial traffic, and record the reported LVGL
# pool/heap/task-stack peak and low-water values) -- it is not passed to
# idf.py. This requires a physically connected board and a real serial
# port -- it is not attempted or simulated here.
run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi

  local case_name=""
  local port=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --port)
        port="${2:-}"
        shift 2
        ;;
      *)
        if [[ -z "$case_name" ]]; then
          case_name="$1"
        fi
        shift
        ;;
    esac
  done

  if [[ -z "$case_name" ]]; then
    echo "error: 'run hardware' requires a case: display-smoke | calibration | peripherals | capacity | cpu-replay" >&2
    return 1
  fi
  if [[ -z "$port" ]]; then
    echo "error: 'run hardware' requires --port <path>; confirm the actual device path (e.g. via 'ls /dev/cu.*'), it changes between plugs" >&2
    return 1
  fi
  if [[ ! -e "$port" ]]; then
    echo "error: port '$port' does not exist. Connect the board and confirm the path before running." >&2
    return 1
  fi

  require_tool idf.py "$(idf_missing_hint)" || return 1

  if [[ "$case_name" == "cpu-replay" ]]; then
    # TB01: flash, replay the scripted CPU timeline on the display, and diff
    # the board's serial log against the simulator's expected transcript. The
    # display itself is still checked by eye (dim at 3 s, Disconnected at 15 s,
    # `Time unavailable` on cold boot); see docs/tickets/TB01.md.
    echo "Flashing the image and replaying the CPU timeline on port '$port' (exclusive board session)."
    (cd "$(dev_root)/firmware" && idf.py -p "$port" flash) || return 1
    python3 "$(dev_root)/tools/hardware/cpu_replay_capture.py" --port "$port" \
      --out "$(dev_root)/firmware/build/cpu-replay-capture.log"
    return
  fi

  echo "Flashing and monitoring the diagnostic image on port '$port', session case '$case_name'."
  echo "This requires an exclusively claimed, physically connected E32R28T board."
  echo "Hold BOOT and tap RESET/EN if the board does not enter the ROM bootloader automatically; see"
  echo "hardware/profiles/lcdwiki-esp32-32e-2.8/development/README.md for the full recovery sequence."
  (cd "$(dev_root)/firmware" && idf.py -p "$port" flash monitor)
}
