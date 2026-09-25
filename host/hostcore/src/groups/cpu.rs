// SPDX-License-Identifier: GPL-3.0-only

//! CPU (always sent, snapshot keys 0-4).

use metrics::Collector;
use platform_macos::cpu::CpuCollector;
use protocol::{CpuSnapshot, Freshness, Snapshot};

use super::{availability, Group, Report};

impl Group for CpuCollector {
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report) {
        let sample = self.collect(now_ms);
        snapshot.cpu = CpuSnapshot {
            availability: availability(sample.availability),
            utilization_per_mille: sample.value.unwrap_or(0),
            source_age_ms: 0,
            freshness: Freshness::Fresh,
            configuration_revision: 0,
        };
        report.status("cpu", snapshot.cpu.availability);
        report.set_cpu(sample.value);
    }

    fn wake(&mut self) {
        self.on_wake();
    }
}
