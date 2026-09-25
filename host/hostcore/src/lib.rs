// SPDX-License-Identifier: GPL-3.0-only

//! Code shared by the per-user daemon (`cyd-hostd`) and the CLI (`cyd-host`):
//! the Pairing Identity store, the serial link runner, the single-instance
//! lock, the local control IPC and the daemon supervisor.

pub mod control;
pub mod daemon;
mod groups;
pub mod identity;
pub mod link;
pub mod lock;
pub mod logging;
pub mod paths;
