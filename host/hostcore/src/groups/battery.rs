// SPDX-License-Identifier: GPL-3.0-only

//! Battery (group key 7).

use metrics::Collector;
use platform_macos::battery::{BatteryCollector, BatteryReading};
use protocol::{Availability, BatterySnapshot, Snapshot};

use super::{without_value, worst, Group, Report};

impl Group for BatteryCollector {
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report) {
        snapshot.battery = battery_snapshot(self.collect(now_ms).value);
        if let Some(g) = &snapshot.battery {
            report.status(
                "battery",
                worst(g.charge_availability, g.condition_availability),
            );
            report.line(format!(
                "battery charge {:?} {:.1}% state {} ({} min)  health {:?} {:.1}% condition {} cycles {}",
                g.charge_availability,
                f64::from(g.charge_per_mille) / 10.0,
                g.state,
                g.time_remaining_min,
                g.condition_availability,
                f64::from(g.max_capacity_per_mille) / 10.0,
                g.condition,
                g.cycle_count
            ));
        }
    }

    fn wake(&mut self) {
        self.on_wake();
    }
}

/// The wire group for one second of battery collection. A half that is not
/// available carries zero values (times and cycle counts the unknown sentinel)
/// and its availability, so a batteryless host is `Absent`, never a 0% charge.
/// A condition source that lacks one field reports that field as 0 (capacity)
/// or the sentinel (cycles): a working battery has no 0% health capacity.
fn battery_snapshot(reading: Option<BatteryReading>) -> Option<BatterySnapshot> {
    let reading = reading?;
    let charge = reading.charge.value;
    let condition = reading.condition.value;
    Some(BatterySnapshot {
        charge_availability: if charge.is_some() {
            Availability::Available
        } else {
            without_value(reading.charge.availability)
        },
        charge_per_mille: charge.map_or(0, |c| c.per_mille.min(1000)),
        state: charge.map_or(0, |c| c.state as u8),
        time_remaining_min: charge
            .and_then(|c| c.time_remaining_min)
            .unwrap_or(protocol::BATTERY_UNKNOWN_U16),
        charge_age_ms: if charge.is_some() {
            reading.charge_age_ms
        } else {
            0
        },
        condition_availability: if condition.is_some() {
            Availability::Available
        } else {
            without_value(reading.condition.availability)
        },
        max_capacity_per_mille: condition
            .and_then(|c| c.max_capacity_per_mille)
            .unwrap_or(0),
        condition: condition.map_or(0, |c| c.condition as u8),
        cycle_count: condition
            .and_then(|c| c.cycle_count)
            .unwrap_or(protocol::BATTERY_UNKNOWN_U16),
        condition_age_ms: if condition.is_some() {
            reading.condition_age_ms
        } else {
            0
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use metrics::battery::{
        BatteryChargeReading, BatteryCondition, BatteryConditionReading, BatteryState,
    };
    use metrics::{MetricAvailability, MetricSample};

    fn reading(
        charge: MetricSample<BatteryChargeReading>,
        condition: MetricSample<BatteryConditionReading>,
    ) -> BatteryReading {
        BatteryReading {
            charge,
            charge_age_ms: 12_000,
            condition,
            condition_age_ms: 600_000,
        }
    }

    #[test]
    fn a_batteryless_host_is_absent_on_the_wire_never_zero_percent() {
        let g = battery_snapshot(Some(reading(
            MetricSample::unavailable(MetricAvailability::Absent),
            MetricSample::unavailable(MetricAvailability::Absent),
        )))
        .unwrap();
        assert_eq!(g.charge_availability, Availability::Absent);
        assert_eq!(g.condition_availability, Availability::Absent);
        assert_eq!((g.charge_per_mille, g.charge_age_ms), (0, 0));
        assert_eq!(g.time_remaining_min, protocol::BATTERY_UNKNOWN_U16);
        assert_eq!(g.cycle_count, protocol::BATTERY_UNKNOWN_U16);
    }

    #[test]
    fn live_halves_carry_values_and_their_own_ages_and_partial_health_is_zero_or_unknown() {
        let g = battery_snapshot(Some(reading(
            MetricSample::available(BatteryChargeReading {
                per_mille: 870,
                state: BatteryState::Charging,
                time_remaining_min: None,
            }),
            MetricSample::available(BatteryConditionReading {
                max_capacity_per_mille: None,
                condition: BatteryCondition::ServiceRecommended,
                cycle_count: None,
            }),
        )))
        .unwrap();
        assert_eq!(
            (g.charge_availability, g.charge_per_mille, g.state),
            (Availability::Available, 870, 1)
        );
        assert_eq!(
            (g.time_remaining_min, g.charge_age_ms),
            (protocol::BATTERY_UNKNOWN_U16, 12_000)
        );
        assert_eq!(
            (
                g.condition_availability,
                g.max_capacity_per_mille,
                g.condition
            ),
            (Availability::Available, 0, 2)
        );
        assert_eq!(
            (g.cycle_count, g.condition_age_ms),
            (protocol::BATTERY_UNKNOWN_U16, 600_000)
        );
    }

    #[test]
    fn a_missing_condition_leaves_the_charge_live_and_errors_carry_no_values() {
        let g = battery_snapshot(Some(reading(
            MetricSample::unavailable(MetricAvailability::Error),
            MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable),
        )))
        .unwrap();
        assert_eq!(g.charge_availability, Availability::Error);
        assert_eq!(
            g.condition_availability,
            Availability::TemporarilyUnavailable
        );
        assert_eq!((g.charge_age_ms, g.condition_age_ms, g.state), (0, 0, 0));
    }
}
