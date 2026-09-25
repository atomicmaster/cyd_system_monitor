// SPDX-License-Identifier: GPL-3.0-only

//! Root storage (TB04): capacity of the filesystem containing `/` and the
//! read/write rate of the physical disk behind it. Pure and fixture-tested;
//! the macOS sources live in `platform-macos`.

use crate::{CounterRateSampler, MetricSample};

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

/// Turns cumulative disk byte counters into rates: the shared
/// [`CounterRateSampler`] discipline (unavailable on the first sample, after a
/// counter decrease from a reset or a remapped device, a non-monotonic clock,
/// a gap longer than [`MAX_RATE_SAMPLE_GAP_MS`](crate::MAX_RATE_SAMPLE_GAP_MS)
/// such as sleep/wake, or zero elapsed time; every call re-baselines). An idle
/// disk is a measured zero rate, not unavailable.
#[derive(Debug, Default)]
pub struct DiskIoSampler(CounterRateSampler<()>);

impl DiskIoSampler {
    pub fn sample(&mut self, counters: DiskCounters, now_ms: u64) -> MetricSample<DiskRate> {
        self.0
            .sample((), [counters.read_bytes, counters.write_bytes], now_ms)
            .map(|[read, write]| DiskRate {
                read_bytes_per_s: read,
                write_bytes_per_s: write,
            })
    }

    /// Forget the baseline: a wake, a hotplug, or a different backing device.
    pub fn reset(&mut self) {
        self.0.reset();
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{MetricAvailability, MAX_RATE_SAMPLE_GAP_MS};

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
            s.sample(counters(100, 100), MAX_RATE_SAMPLE_GAP_MS + 1)
                .value,
            None,
            "sleep/wake gap"
        );
        assert!(s
            .sample(counters(200, 100), MAX_RATE_SAMPLE_GAP_MS + 1001)
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
}
