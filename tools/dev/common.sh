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

# `idf.py` failing `command -v` does not mean ESP-IDF is uninstalled: the
# far more common case on a dev machine is that it's installed but this
# particular shell never sourced its export.sh (ESP-IDF is not on PATH by
# default -- unlike cmake/ninja/python3, it has no system-wide install). An
# agent whose tool calls each start a fresh subshell hits this constantly,
# since `source .../export.sh` in one command does not carry over to the
# next command; it must run in the *same* shell invocation as the idf.py
# call that needs it. Rather than let that read as "not installed" and
# stall, check the usual install locations first and hand back the exact
# command to run.
idf_export_candidate() {
  local candidates=()
  [[ -n "${IDF_PATH:-}" ]] && candidates+=("$IDF_PATH/export.sh")
  candidates+=("$HOME/esp/esp-idf/export.sh" "/opt/esp/esp-idf/export.sh")
  local path
  for path in "${candidates[@]}"; do
    if [[ -f "$path" ]]; then
      echo "$path"
      return 0
    fi
  done
  return 1
}

idf_missing_hint() {
  local export_sh
  if export_sh=$(idf_export_candidate); then
    local version expected candidate
    version=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    expected="$HOME/.espressif/python_env/idf5.3_py${version}_env"
    if [[ ! -d "$expected" ]]; then
      for candidate in "$HOME"/.espressif/python_env/idf5.3_py3.*_env; do
        if [[ -d "$candidate" ]]; then
          echo "ESP-IDF Python env missing for Python $version. Run: export IDF_PYTHON_ENV_PATH=$candidate before sourcing export.sh; alternatively run $(dirname "$export_sh")/install.sh esp32."
          break
        fi
      done
    fi
    echo "ESP-IDF looks installed at $(dirname "$export_sh") but is not sourced in this shell. Run: source $export_sh && ./dev <command> firmware ... -- in the SAME shell invocation, since sourcing export.sh and running idf.py in separate commands loses the exported PATH/IDF_PATH."
  else
    echo "install ESP-IDF and 'source \$IDF_PATH/export.sh' (https://docs.espressif.com/projects/esp-idf)"
  fi
}

# Differential C++/Rust protocol parity gate (protocol/tools/parity_diff.py).
# Every lane that compiles or changes a protocol codec runs it, so a codec
# change cannot pass its own lane with the two implementations misaligned.
# Under `./dev check all` only the protocol lane runs it, once.
run_parity_gate() {
  if [[ "${CYD_IN_ALL:-}" == "1" && "${1:-}" != "protocol" ]]; then
    return 0
  fi
  require_tool python3 "install Python 3" || return 1
  require_tool cargo "install Rust via https://rustup.rs" || return 1
  require_tool cmake "brew install cmake" || return 1
  python3 "$(dev_root)/protocol/tools/parity_diff.py"
}
