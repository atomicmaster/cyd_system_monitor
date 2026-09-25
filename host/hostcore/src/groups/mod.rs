// SPDX-License-Identifier: GPL-3.0-only

//! Each metric group's Host-side glue, one module per group: collect one
//! second from its `metrics::Collector`, map it to its wire group, say what
//! the status summary and the CLI log report about it, and forget rates and
//! caches on a wake. The list of groups, in snapshot and report order, is
//! [`all`]; adding a group is one module here and one line there.

use metrics::wifi_context::WifiContextProvider;
use metrics::MetricAvailability;
use platform_macos::agents::AgentsCollector;
use platform_macos::battery::BatteryCollector;
use platform_macos::cpu::CpuCollector;
use platform_macos::ram::RamCollector;
use platform_macos::storage::StorageCollector;
use protocol::{Availability, Snapshot};

mod agents;
mod battery;
mod cpu;
mod network;
mod ram;
mod storage;

/// One metric group on the Host.
pub(crate) trait Group {
    /// Collects one second: writes the group into `snapshot` and records
    /// what the status summary and the CLI log say about it in `report`.
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report);
    /// The Mac slept (`metrics::Collector::on_wake`).
    fn wake(&mut self);
}

/// Every metric group the Host sends, in snapshot and report order.
pub(crate) fn all(provider: Box<dyn WifiContextProvider>) -> Vec<Box<dyn Group>> {
    vec![
        Box::new(CpuCollector::new()),
        Box::new(RamCollector::new()),
        Box::new(StorageCollector::new()),
        Box::new(BatteryCollector::new()),
        Box::new(AgentsCollector::new()),
        Box::new(platform_macos::network::collector(provider)),
    ]
}

/// What one second of collection says, beyond the wire groups.
#[derive(Debug, Default)]
pub struct Report {
    availability: Vec<(&'static str, Availability)>,
    lines: Vec<LogLine>,
    /// The CLI's closing "cpu ... ram ..." line.
    cpu_per_mille: Option<u16>,
    ram_bytes: Option<(u64, u64)>,
}

#[derive(Debug)]
struct LogLine {
    text: String,
    /// The WiFi context provider's drop counters follow this line.
    provider_drops: bool,
}

impl Report {
    /// The group's availability for the status summary.
    pub(crate) fn status(&mut self, name: &'static str, availability: Availability) {
        self.availability.push((name, availability));
    }

    /// One line of the CLI's per-second report. States and numbers only:
    /// never a path, process name, MAC address, SSID or BSSID.
    pub(crate) fn line(&mut self, text: String) {
        self.lines.push(LogLine {
            text,
            provider_drops: false,
        });
    }

    /// A line that the provider's drop counters are appended to.
    pub(crate) fn line_with_provider_drops(&mut self, text: String) {
        self.lines.push(LogLine {
            text,
            provider_drops: true,
        });
    }

    pub(crate) fn set_cpu(&mut self, per_mille: Option<u16>) {
        self.cpu_per_mille = per_mille;
    }

    pub(crate) fn set_ram(&mut self, used_total: Option<(u64, u64)>) {
        self.ram_bytes = used_total;
    }

    /// Per-metric-group availability, in list order.
    pub fn group_availability(&self) -> Vec<(&'static str, Availability)> {
        self.availability.clone()
    }

    /// The per-second report: each group's line, then CPU and RAM.
    pub fn log_lines(&self, drops: Option<String>) -> Vec<String> {
        let mut drops = drops;
        let mut lines: Vec<String> = self
            .lines
            .iter()
            .map(|line| match line.provider_drops {
                true => format!("{}{}", line.text, drops.take().unwrap_or_default()),
                false => line.text.clone(),
            })
            .collect();
        lines.push(match (self.cpu_per_mille, self.ram_bytes) {
            (Some(v), Some((used, total))) => format!(
                "cpu {:.1}%  ram {:.1} / {:.1} GB",
                f64::from(v) / 10.0,
                used as f64 / (1u64 << 30) as f64,
                total as f64 / (1u64 << 30) as f64
            ),
            (Some(v), None) => format!("cpu {:.1}%  ram unavailable", f64::from(v) / 10.0),
            (None, _) => "cpu unavailable".to_string(),
        });
        lines
    }
}

/// The wire availability of a metric availability.
pub(crate) fn availability(a: MetricAvailability) -> Availability {
    match a {
        MetricAvailability::Available => Availability::Available,
        MetricAvailability::Unsupported => Availability::Unsupported,
        MetricAvailability::Absent => Availability::Absent,
        MetricAvailability::TemporarilyUnavailable => Availability::TemporarilyUnavailable,
        MetricAvailability::Error => Availability::Error,
    }
}

/// The wire availability of a sample without a value: "available" with no
/// value behind it is an error.
pub(crate) fn without_value(a: MetricAvailability) -> Availability {
    match availability(a) {
        Availability::Available => Availability::Error,
        other => other,
    }
}

/// The less healthy of two independent halves of one group.
pub(crate) fn worst(a: Availability, b: Availability) -> Availability {
    if a == Availability::Available {
        b
    } else {
        a
    }
}
