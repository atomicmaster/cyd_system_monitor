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
}
