// SPDX-License-Identifier: GPL-3.0-only

//! Portable Host Metric seam. Platform adapters (see `platform-macos`) and
//! concrete per-metric collectors are added by tracer tickets; this crate
//! only fixes the shared contract they implement against.
//!
//! Availability, freshness, and adapter enablement are kept separate: a
//! missing value never collapses into a measured zero (see
//! `docs/host-metrics.md`).

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
