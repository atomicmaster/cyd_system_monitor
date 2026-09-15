<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Build the daemon in Rust

The main host component will be a Rust binary built around the portable Host Metric contract, with a macOS adapter for the MVP and later platform adapters for Linux and Windows. Its CLI will manage a per-user LaunchAgent and expose status, configuration, firmware-update, and recovery commands. A separately installed fixed-purpose privileged helper is permitted only for V2 Apple Activity Diagnostics; neither the helper nor GPU/ANE collection is an MVP requirement. Rust adds a separate toolchain from the ESP-IDF C++ firmware, but provides memory-safe protocol parsing and a credible cross-platform path without making macOS-specific Swift the core.
