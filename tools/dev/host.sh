# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `host` target (Rust daemon + CLI workspace).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_host_dir() { echo "$(dev_root)/host"; }

doctor() {
  report_tool cargo "install Rust via https://rustup.rs"
  report_tool rustfmt "rustup component add rustfmt"
  report_tool cargo-clippy "rustup component add clippy"
}

build() {
  require_tool cargo "install Rust via https://rustup.rs" || return 1
  (cd "$(_host_dir)" && cargo build)
}

check() {
  require_tool cargo "install Rust via https://rustup.rs" || return 1
  # The link-parity tests drive the real firmware HostLink through link_cli.
  require_tool cmake "brew install cmake" || return 1
  require_tool ninja "brew install ninja" || return 1
  local link_dir="$(dev_root)/firmware/domain/tests"
  cmake -S "$link_dir" -B "$link_dir/build" -G Ninja >/dev/null
  cmake --build "$link_dir/build" --target link_cli
  (
    cd "$(_host_dir)"
    cargo fmt --check
    cargo clippy --all-targets -- -D warnings
    cargo test
  )
  run_parity_gate host
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  require_tool cargo "install Rust via https://rustup.rs" || return 1
  (cd "$(_host_dir)" && cargo run --bin cyd-host -- "$@")
}
