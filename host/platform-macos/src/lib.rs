// SPDX-License-Identifier: GPL-3.0-only

//! Home for macOS-specific Host Metric collection, permissions, and service
//! management (see `docs/macos-components.md`). Empty at M0: concrete
//! collectors (Mach CPU, RAM, filesystem, network, battery, agent presence)
//! and the Location/Association Context workflow are added by their
//! tracer tickets, each behind `metrics::MetricSample`.
