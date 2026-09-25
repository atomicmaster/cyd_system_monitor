// SPDX-License-Identifier: GPL-3.0-only

//! Physical RAM Occupied (group key 5).

use metrics::Collector;
use platform_macos::ram::RamCollector;
use protocol::{Freshness, RamSnapshot, Snapshot};

use super::{availability, Group, Report};

impl Group for RamCollector {
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report) {
        let sample = self.collect(now_ms);
        let ram = RamSnapshot {
            availability: availability(sample.availability),
            used_bytes: sample.value.map_or(0, |r| r.used_bytes),
            total_bytes: sample.value.map_or(0, |r| r.total_bytes),
            source_age_ms: 0,
            freshness: Freshness::Fresh,
        };
        report.status("ram", ram.availability);
        report.set_ram(sample.value.map(|r| (r.used_bytes, r.total_bytes)));
        snapshot.ram = Some(ram);
    }

    fn wake(&mut self) {
        self.on_wake();
    }
}
