// SPDX-License-Identifier: GPL-3.0-only

//! Home for macOS-specific Host Metric collection, permissions, and service
//! management (see `docs/macos-components.md`). Concrete collectors are
//! added by their tracer tickets, each behind `metrics::MetricSample`.

pub mod agents;
pub mod battery;
#[cfg(target_os = "macos")]
mod cf;
pub mod cpu;
pub mod ipc;
pub mod network;
pub mod ram;
pub mod storage;
pub mod time;
pub mod wifi_context_server;
