// SPDX-License-Identifier: GPL-3.0-only

//! Per-user LaunchAgent skeleton. M0 proves a clean exit with no external
//! contact; USB discovery, pairing, and metric collection are TB02/TB03+
//! work. See `docs/macos-components.md`.

fn main() {
    println!("cyd-hostd: skeleton daemon exiting (no collectors wired at M0)");
}
