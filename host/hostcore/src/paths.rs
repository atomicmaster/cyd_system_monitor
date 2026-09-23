// SPDX-License-Identifier: GPL-3.0-only

//! Per-user directories. Environment overrides exist for development and
//! tests; nothing here creates anything.

use std::path::PathBuf;

/// The launchd label of the per-user daemon. A placeholder until R01 fixes the
/// product's reverse-DNS identifier.
pub const SERVICE_LABEL: &str = "com.cydsystemmonitor.hostd";

fn home() -> PathBuf {
    std::env::var_os("HOME").map_or_else(|| "/tmp".into(), PathBuf::from)
}

/// `$CYD_RUN_DIR`, else `~/Library/Application Support/CYD System Monitor/run`:
/// the lock, the provider socket and the control socket.
pub fn run_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("CYD_RUN_DIR") {
        return dir.into();
    }
    home().join("Library/Application Support/CYD System Monitor/run")
}

/// `$CYD_LOG_DIR`, else `~/Library/Logs/CYD System Monitor`.
pub fn log_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("CYD_LOG_DIR") {
        return dir.into();
    }
    home().join("Library/Logs/CYD System Monitor")
}

/// `~/Library/LaunchAgents`.
pub fn launch_agents_dir() -> PathBuf {
    home().join("Library/LaunchAgents")
}
