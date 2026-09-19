// SPDX-License-Identifier: GPL-3.0-only

//! Home for macOS-specific Host Metric collection, permissions, and service
//! management (see `docs/macos-components.md`). Concrete collectors are
//! added by their tracer tickets, each behind `metrics::MetricSample`.

pub mod cpu;
