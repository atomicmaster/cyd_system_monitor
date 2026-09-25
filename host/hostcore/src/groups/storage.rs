// SPDX-License-Identifier: GPL-3.0-only

//! Root storage (group key 6).

use metrics::Collector;
use platform_macos::storage::{StorageCollector, StorageReading};
use protocol::{Availability, Snapshot, StorageSnapshot};

use super::{availability, without_value, worst, Group, Report};

impl Group for StorageCollector {
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report) {
        snapshot.storage = storage_snapshot(self.collect(now_ms).value);
        if let Some(g) = &snapshot.storage {
            report.status("storage", worst(g.capacity_availability, g.io_availability));
            report.line(format!(
                "storage {:.1} / {:.1} GB  read {} B/s  write {} B/s",
                (g.total_bytes - g.available_bytes) as f64 / 1e9,
                g.total_bytes as f64 / 1e9,
                g.read_bytes_per_s,
                g.write_bytes_per_s
            ));
        }
    }

    fn wake(&mut self) {
        self.on_wake();
    }
}

/// The wire group for one second of storage collection. A half that is not
/// available carries zero values and its availability; capacity and I/O are
/// independent.
fn storage_snapshot(reading: Option<StorageReading>) -> Option<StorageSnapshot> {
    let reading = reading?;
    let capacity = reading
        .capacity
        .value
        .filter(|c| c.available_bytes <= c.total_bytes);
    let io = reading.io.value;
    Some(StorageSnapshot {
        capacity_availability: match capacity {
            Some(_) => Availability::Available,
            None => without_value(reading.capacity.availability),
        },
        total_bytes: capacity.map_or(0, |c| c.total_bytes),
        available_bytes: capacity.map_or(0, |c| c.available_bytes),
        capacity_age_ms: if capacity.is_some() {
            reading.capacity_age_ms
        } else {
            0
        },
        io_availability: availability(reading.io.availability),
        read_bytes_per_s: io.map_or(0, |r| r.read_bytes_per_s),
        write_bytes_per_s: io.map_or(0, |r| r.write_bytes_per_s),
        io_age_ms: 0,
    })
}
