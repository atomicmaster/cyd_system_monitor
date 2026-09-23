// SPDX-License-Identifier: GPL-3.0-only

//! Portable Host Metric seam. Platform adapters (see `platform-macos`) and
//! concrete per-metric collectors are added by tracer tickets; this crate
//! only fixes the shared contract they implement against.
//!
//! Availability, freshness, and adapter enablement are kept separate: a
//! missing value never collapses into a measured zero (see
//! `docs/host-metrics.md`).

pub mod agents;
pub mod battery;
pub mod network;
pub mod wifi_context;

/// Whether a Host Metric's current value can be trusted, independent of
/// how fresh it is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MetricAvailability {
    Available,
    Unsupported,
    Absent,
    TemporarilyUnavailable,
    Error,
}

/// A metric's value paired with its availability. `value` is only
/// meaningful when `availability` is `Available`.
#[derive(Debug, Clone, PartialEq)]
pub struct MetricSample<T> {
    pub availability: MetricAvailability,
    pub value: Option<T>,
}

impl<T> MetricSample<T> {
    pub fn available(value: T) -> Self {
        Self {
            availability: MetricAvailability::Available,
            value: Some(value),
        }
    }

    pub fn unavailable(availability: MetricAvailability) -> Self {
        debug_assert_ne!(availability, MetricAvailability::Available);
        Self {
            availability,
            value: None,
        }
    }
}

/// One Host metric collector. The session loop calls `collect` once per
/// second with a monotonic clock and puts the result in the next complete
/// snapshot; a collector never opens ports, blocks on slow sources, or
/// decides how its value is displayed. A slow source (a subprocess, a
/// permission prompt) keeps its own cache and returns the last sample with
/// its age, or an unavailable sample, rather than stalling this call.
pub trait Collector {
    type Reading;
    fn collect(&mut self, now_ms: u64) -> MetricSample<Self::Reading>;
}

/// Physical RAM Occupied as two byte counts (total minus free pages).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RamReading {
    pub used_bytes: u64,
    pub total_bytes: u64,
}

/// Capacity and available bytes of the filesystem containing `/`. For APFS
/// this is the container's space, shared by all its volumes, so volumes are
/// never summed (`docs/host-metrics.md`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StorageCapacityReading {
    pub total_bytes: u64,
    pub available_bytes: u64,
}

/// Cumulative bytes read and written by a physical storage driver.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DiskCounters {
    pub read_bytes: u64,
    pub write_bytes: u64,
}

/// Disk throughput in bytes per second.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DiskRate {
    pub read_bytes_per_s: u64,
    pub write_bytes_per_s: u64,
}

/// Turns cumulative disk byte counters into rates, with the same discipline as
/// `CpuSampler`: unavailable on the first sample, after a counter decrease
/// (reset or a remapped device), a non-monotonic clock, a gap longer than
/// `CPU_MAX_SAMPLE_GAP_MS` (sleep/wake), or zero elapsed time. Every call
/// re-baselines, so the next second can yield a rate. An idle disk is a
/// measured zero rate, not unavailable.
#[derive(Debug, Default)]
pub struct DiskIoSampler {
    baseline: Option<(DiskCounters, u64)>,
}

impl DiskIoSampler {
    pub fn sample(&mut self, counters: DiskCounters, now_ms: u64) -> MetricSample<DiskRate> {
        let previous = self.baseline.replace((counters, now_ms));
        let Some((before, before_ms)) = previous else {
            return MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable);
        };
        let gap_ok = now_ms > before_ms && now_ms - before_ms <= CPU_MAX_SAMPLE_GAP_MS;
        let monotonic =
            counters.read_bytes >= before.read_bytes && counters.write_bytes >= before.write_bytes;
        if !gap_ok || !monotonic {
            return MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable);
        }
        let elapsed = u128::from(now_ms - before_ms);
        let per_s =
            |delta: u64| (u128::from(delta) * 1000 / elapsed).min(u128::from(u64::MAX)) as u64;
        MetricSample::available(DiskRate {
            read_bytes_per_s: per_s(counters.read_bytes - before.read_bytes),
            write_bytes_per_s: per_s(counters.write_bytes - before.write_bytes),
        })
    }

    /// Forget the baseline: a wake, a hotplug, or a different backing device.
    pub fn reset(&mut self) {
        self.baseline = None;
    }
}

/// The whole-disk BSD name of a BSD name or `/dev` path: `disk3s1s1` and
/// `/dev/disk3s5` are on `disk3`. `None` for anything that is not a `diskN`
/// name (network mounts, `map auto_home`).
pub fn whole_disk_name(bsd_name: &str) -> Option<&str> {
    let name = bsd_name.strip_prefix("/dev/").unwrap_or(bsd_name);
    let rest = name.strip_prefix("disk")?;
    let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
    if digits == 0 {
        return None;
    }
    // What follows the whole-disk number must be slices: `s<digits>` repeated.
    let mut tail = &rest[digits..];
    while let Some(after) = tail.strip_prefix('s') {
        let n = after.bytes().take_while(u8::is_ascii_digit).count();
        if n == 0 {
            return None;
        }
        tail = &after[n..];
    }
    tail.is_empty().then(|| &name[..4 + digits])
}

/// Follows a volume's BSD name to the physical whole disk. `store_of` says
/// which BSD name backs a whole disk, if any: an APFS synthesized container
/// (`disk3`) is backed by a partition on a physical disk (`disk0s2`); a
/// physical disk has none. Every volume of a container (`disk3s1s1`,
/// `disk3s5`) therefore resolves to the same physical disk, so their I/O is
/// one device's and volumes are never summed.
pub fn physical_disk_name(
    bsd_name: &str,
    store_of: &dyn Fn(&str) -> Option<String>,
) -> Option<String> {
    let mut current = whole_disk_name(bsd_name)?.to_string();
    for _ in 0..4 {
        let Some(store) = store_of(&current) else {
            return Some(current);
        };
        let next = whole_disk_name(&store)?;
        if next == current {
            return Some(current);
        }
        current = next.to_string();
    }
    None // a chain this deep is a loop or a mistake, not a disk
}

/// A collector that replays a fixed script, one entry per call and then the
/// last entry forever. Fixture adapter for scenarios and tests.
pub struct ScriptedCollector<T: Clone> {
    script: Vec<MetricSample<T>>,
    next: usize,
}

impl<T: Clone> ScriptedCollector<T> {
    pub fn new(script: Vec<MetricSample<T>>) -> Self {
        assert!(!script.is_empty(), "a script needs at least one sample");
        Self { script, next: 0 }
    }
}

impl<T: Clone> Collector for ScriptedCollector<T> {
    type Reading = T;
    fn collect(&mut self, _now_ms: u64) -> MetricSample<T> {
        let sample = self.script[self.next.min(self.script.len() - 1)].clone();
        self.next += 1;
        sample
    }
}

/// Cumulative CPU tick counters as reported by the OS (Mach
/// `HOST_CPU_LOAD_INFO`: 32-bit counters, so a decrease means a reset).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CpuTicks {
    pub user: u32,
    pub system: u32,
    pub idle: u32,
    pub nice: u32,
}

/// Longest gap between samples that still yields a rate. A longer gap
/// (sleep/wake, a stalled collector) makes the delta meaningless, so the
/// sampler re-baselines instead of reporting an average over the gap.
pub const CPU_MAX_SAMPLE_GAP_MS: u64 = 5_000;

/// Turns cumulative tick counters into utilization per mille. Rates are
/// unavailable until a valid baseline exists: on the first sample, after
/// a counter reset, a non-monotonic clock, an over-long gap, or an
/// interval with no elapsed ticks. `now_ms` must come from a monotonic
/// clock.
#[derive(Debug, Default)]
pub struct CpuSampler {
    baseline: Option<(CpuTicks, u64)>,
}

impl CpuSampler {
    pub fn sample(&mut self, ticks: CpuTicks, now_ms: u64) -> MetricSample<u16> {
        let previous = self.baseline.replace((ticks, now_ms));
        let Some((before, before_ms)) = previous else {
            return MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable);
        };
        let gap_ok = now_ms > before_ms && now_ms - before_ms <= CPU_MAX_SAMPLE_GAP_MS;
        let monotonic = ticks.user >= before.user
            && ticks.system >= before.system
            && ticks.idle >= before.idle
            && ticks.nice >= before.nice;
        if !gap_ok || !monotonic {
            return MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable);
        }
        let busy = u64::from(ticks.user - before.user)
            + u64::from(ticks.nice - before.nice)
            + u64::from(ticks.system - before.system);
        let total = busy + u64::from(ticks.idle - before.idle);
        if total == 0 {
            return MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable);
        }
        MetricSample::available(((busy * 1000 + total / 2) / total) as u16)
    }

    /// Forget the baseline, e.g. on a detected sleep/wake.
    pub fn reset(&mut self) {
        self.baseline = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn available_sample_carries_its_value() {
        let sample = MetricSample::available(42u64);
        assert_eq!(sample.availability, MetricAvailability::Available);
        assert_eq!(sample.value, Some(42));
    }

    #[test]
    fn unavailable_sample_carries_no_value() {
        let sample: MetricSample<u64> =
            MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable);
        assert_eq!(
            sample.availability,
            MetricAvailability::TemporarilyUnavailable
        );
        assert_eq!(sample.value, None);
    }

    fn ticks(user: u32, system: u32, idle: u32, nice: u32) -> CpuTicks {
        CpuTicks {
            user,
            system,
            idle,
            nice,
        }
    }

    #[test]
    fn cpu_first_sample_is_unavailable_not_zero() {
        let mut s = CpuSampler::default();
        let r = s.sample(ticks(10, 10, 80, 0), 0);
        assert_eq!(r.availability, MetricAvailability::TemporarilyUnavailable);
        assert_eq!(r.value, None);
    }

    #[test]
    fn cpu_rate_is_busy_over_total_in_per_mille() {
        let mut s = CpuSampler::default();
        s.sample(ticks(0, 0, 0, 0), 0);
        let r = s.sample(ticks(20, 10, 60, 10), 1000);
        assert_eq!(r.value, Some(400));
        let r = s.sample(ticks(20, 10, 160, 10), 2000);
        assert_eq!(r.value, Some(0)); // a measured zero is still available
        assert_eq!(r.availability, MetricAvailability::Available);
    }

    #[test]
    fn cpu_counter_reset_rebaselines() {
        let mut s = CpuSampler::default();
        s.sample(ticks(100, 100, 100, 0), 0);
        let r = s.sample(ticks(1, 1, 1, 0), 1000);
        assert_eq!(r.availability, MetricAvailability::TemporarilyUnavailable);
        assert_eq!(s.sample(ticks(11, 1, 11, 0), 2000).value, Some(500));
    }

    #[test]
    fn cpu_long_gap_or_backwards_clock_rebaselines() {
        let mut s = CpuSampler::default();
        s.sample(ticks(0, 0, 0, 0), 0);
        let r = s.sample(ticks(50, 0, 50, 0), CPU_MAX_SAMPLE_GAP_MS + 1);
        assert_eq!(r.value, None);
        assert_eq!(
            s.sample(ticks(60, 0, 90, 0), CPU_MAX_SAMPLE_GAP_MS + 1001)
                .value,
            Some(200)
        );
        assert_eq!(s.sample(ticks(70, 0, 130, 0), 10).value, None);
    }

    #[test]
    fn cpu_no_elapsed_ticks_is_unavailable() {
        let mut s = CpuSampler::default();
        s.sample(ticks(5, 5, 5, 5), 0);
        assert_eq!(s.sample(ticks(5, 5, 5, 5), 1000).value, None);
    }

    #[test]
    fn cpu_reset_drops_the_baseline() {
        let mut s = CpuSampler::default();
        s.sample(ticks(0, 0, 0, 0), 0);
        s.reset();
        assert_eq!(s.sample(ticks(10, 0, 10, 0), 1000).value, None);
    }

    fn counters(read: u64, write: u64) -> DiskCounters {
        DiskCounters {
            read_bytes: read,
            write_bytes: write,
        }
    }

    #[test]
    fn disk_first_sample_is_unavailable_not_zero() {
        let mut s = DiskIoSampler::default();
        let r = s.sample(counters(1000, 1000), 0);
        assert_eq!(r.availability, MetricAvailability::TemporarilyUnavailable);
        assert_eq!(r.value, None);
    }

    #[test]
    fn disk_rate_is_delta_per_second_and_idle_is_a_measured_zero() {
        let mut s = DiskIoSampler::default();
        s.sample(counters(0, 0), 0);
        let r = s.sample(counters(12_300_000, 4_500_000), 1000);
        assert_eq!(
            r.value,
            Some(DiskRate {
                read_bytes_per_s: 12_300_000,
                write_bytes_per_s: 4_500_000
            })
        );
        // A slightly late tick scales by the real elapsed time.
        let r = s.sample(counters(12_300_000 + 3_000, 4_500_000), 3000);
        assert_eq!(r.value.unwrap().read_bytes_per_s, 1_500);
        let r = s.sample(counters(12_303_000, 4_500_000), 4000);
        assert_eq!(r.availability, MetricAvailability::Available);
        assert_eq!(r.value.unwrap().read_bytes_per_s, 0);
    }

    #[test]
    fn disk_counter_reset_rebaselines_for_either_direction() {
        for reset in [counters(5, 5_000_000), counters(5_000_000, 5)] {
            let mut s = DiskIoSampler::default();
            s.sample(counters(1_000_000, 1_000_000), 0);
            let r = s.sample(reset, 1000);
            assert_eq!(r.availability, MetricAvailability::TemporarilyUnavailable);
            let r = s.sample(
                counters(reset.read_bytes + 10, reset.write_bytes + 20),
                2000,
            );
            assert_eq!(
                r.value,
                Some(DiskRate {
                    read_bytes_per_s: 10,
                    write_bytes_per_s: 20
                })
            );
        }
    }

    #[test]
    fn disk_long_gap_backwards_clock_and_zero_elapsed_rebaseline() {
        let mut s = DiskIoSampler::default();
        s.sample(counters(0, 0), 0);
        assert_eq!(
            s.sample(counters(100, 100), CPU_MAX_SAMPLE_GAP_MS + 1)
                .value,
            None,
            "sleep/wake gap"
        );
        assert!(s
            .sample(counters(200, 100), CPU_MAX_SAMPLE_GAP_MS + 1001)
            .value
            .is_some());
        assert_eq!(
            s.sample(counters(300, 100), 10).value,
            None,
            "backwards clock"
        );
        assert_eq!(s.sample(counters(400, 100), 10).value, None, "zero elapsed");
        assert!(s.sample(counters(500, 100), 1010).value.is_some());
    }

    #[test]
    fn disk_reset_drops_the_baseline_and_huge_deltas_do_not_overflow() {
        let mut s = DiskIoSampler::default();
        s.sample(counters(0, 0), 0);
        s.reset();
        assert_eq!(s.sample(counters(10, 10), 1000).value, None);
        let mut s = DiskIoSampler::default();
        s.sample(counters(0, 0), 0);
        let r = s.sample(counters(u64::MAX, u64::MAX), 1);
        assert_eq!(r.value.unwrap().read_bytes_per_s, u64::MAX);
    }

    #[test]
    fn bsd_names_map_to_their_whole_disk() {
        assert_eq!(whole_disk_name("disk3s1s1"), Some("disk3"));
        assert_eq!(whole_disk_name("/dev/disk3s5"), Some("disk3"));
        assert_eq!(whole_disk_name("disk0"), Some("disk0"));
        assert_eq!(whole_disk_name("disk12s2"), Some("disk12"));
        for bad in [
            "",
            "disk",
            "diskx",
            "disk3s",
            "disk3x",
            "disk3s1x",
            "map auto_home",
            "//srv/x",
        ] {
            assert_eq!(whole_disk_name(bad), None, "{bad}");
        }
    }

    #[test]
    fn apfs_volumes_resolve_to_one_physical_disk_and_are_not_summed() {
        // disk3 is the synthesized APFS container stored on disk0s2.
        let store_of = |whole: &str| (whole == "disk3").then(|| "disk0s2".to_string());
        for volume in ["disk3s1s1", "disk3s1", "disk3s5", "/dev/disk3s6", "disk3"] {
            assert_eq!(
                physical_disk_name(volume, &store_of).as_deref(),
                Some("disk0"),
                "{volume}"
            );
        }
        // A plain partition of a physical disk resolves to itself.
        assert_eq!(
            physical_disk_name("disk0s2", &store_of).as_deref(),
            Some("disk0")
        );
        assert_eq!(physical_disk_name("map auto_home", &store_of), None);
        // A cycle or an over-deep chain is refused, not looped on.
        let loops = |whole: &str| {
            Some(
                if whole == "disk1" {
                    "disk2s1"
                } else {
                    "disk1s1"
                }
                .to_string(),
            )
        };
        assert_eq!(physical_disk_name("disk1s1", &loops), None);
    }

    #[test]
    fn scripted_collectors_replay_then_hold_the_last_sample() {
        let mut ram = ScriptedCollector::new(vec![
            MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable),
            MetricSample::available(RamReading {
                used_bytes: 8,
                total_bytes: 16,
            }),
        ]);
        assert_eq!(ram.collect(0).value, None);
        let second = ram.collect(1000);
        assert_eq!(
            second.value,
            Some(RamReading {
                used_bytes: 8,
                total_bytes: 16
            })
        );
        assert_eq!(ram.collect(2000), second, "the last sample is held");
    }
}
