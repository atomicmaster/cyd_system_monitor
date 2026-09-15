# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `protocol` target (schema generation + consumers).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_protocol_dir() { echo "$(dev_root)/protocol"; }
_cpp_out() { echo "$(_protocol_dir)/generated/cpp/protocol_generated.hpp"; }
_rust_out() { echo "$(_protocol_dir)/generated/rust/src/lib.rs"; }

doctor() {
  report_tool python3 "install Python 3 (used by the schema generator)"
  report_tool cmake "brew install cmake"
  report_tool ninja "brew install ninja"
  report_tool cargo "install Rust via https://rustup.rs"
}

_generate() {
  require_tool python3 "install Python 3" || return 1
  python3 "$(_protocol_dir)/tools/generate.py" \
    --schema-dir "$(_protocol_dir)/schema" \
    --cpp-out "$(_cpp_out)" \
    --rust-out "$(_rust_out)" \
    "$@"
}

build() {
  _generate
}

# Regenerates and fails on drift, then builds/runs both consumers.
check() {
  require_tool cmake "brew install cmake" || return 1
  require_tool ninja "brew install ninja" || return 1
  require_tool cargo "install Rust via https://rustup.rs" || return 1

  _generate --check || {
    echo "error: generated output is stale; run './dev build protocol' and commit the result" >&2
    return 1
  }

  local cpp_test_dir="$(_protocol_dir)/tests/cpp"
  cmake -S "$cpp_test_dir" -B "$cpp_test_dir/build" -G Ninja
  cmake --build "$cpp_test_dir/build"
  "$cpp_test_dir/build/protocol_generated_tests"

  (cd "$(_protocol_dir)/generated/rust" && cargo test)
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  _generate "$@"
}
