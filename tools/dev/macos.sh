# SPDX-License-Identifier: GPL-3.0-only
# Dispatch script for the `macos` target (SwiftUI setup app).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_macos_dir() { echo "$(dev_root)/macos/setup-app"; }

doctor() {
  report_tool swift "install Xcode / Command Line Tools"
  report_tool xcodebuild "install Xcode (needed for signing later; not required at M0)"
  report_tool swift-format "brew install swift-format (optional at M0)"
}

build() {
  require_tool swift "install Xcode / Command Line Tools" || return 1
  (cd "$(_macos_dir)" && swift build)
}

check() {
  require_tool swift "install Xcode / Command Line Tools" || return 1
  if command -v swift-format >/dev/null 2>&1; then
    (cd "$(_macos_dir)" && swift-format lint --recursive Sources)
  else
    echo "note: swift-format not installed; skipping lint (see ./dev doctor)"
  fi
  build
}

run() {
  if [[ "${1:-}" == "--" ]]; then shift; fi
  require_tool swift "install Xcode / Command Line Tools" || return 1
  (cd "$(_macos_dir)" && swift run "$@")
}
