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
  (
    cd "$(_host_dir)"
    cargo fmt --check
    cargo clippy --all-targets -- -D warnings
    cargo test
  )
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  require_tool cargo "install Rust via https://rustup.rs" || return 1
  (cd "$(_host_dir)" && cargo run --bin cyd-host -- "$@")
}
