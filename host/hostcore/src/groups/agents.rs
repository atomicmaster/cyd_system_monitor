// SPDX-License-Identifier: GPL-3.0-only

//! Agent Activity presence (group key 8).

use metrics::Collector;
use platform_macos::agents::{AgentsCollector, AgentsReading};
use protocol::{AgentsSnapshot, Availability, Snapshot};

use super::{without_value, Group, Report};

impl Group for AgentsCollector {
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report) {
        let agents = agents_snapshot(self.collect(now_ms));
        report.status("agents", agents.availability);
        report.line(format!(
            "agents {:?}: codex {} claude {} grok {}",
            agents.availability, agents.state[0], agents.state[1], agents.state[2]
        ));
        snapshot.agents = Some(agents);
    }

    fn wake(&mut self) {
        self.on_wake();
    }
}

/// The wire group for one second of agent presence. A source that is not
/// available carries all-zero states and its availability, so a failed process
/// listing is never "none running".
fn agents_snapshot(sample: metrics::MetricSample<AgentsReading>) -> AgentsSnapshot {
    match sample.value {
        Some(reading) => AgentsSnapshot {
            availability: Availability::Available,
            source_age_ms: reading.age_ms,
            state: reading.states.map(|s| s as u8),
        },
        None => AgentsSnapshot {
            availability: without_value(sample.availability),
            source_age_ms: 0,
            state: [0; 3],
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use metrics::MetricAvailability;

    #[test]
    fn agent_presence_maps_to_the_wire_and_an_unavailable_source_is_all_zero() {
        use metrics::agents::AgentState;
        let g = agents_snapshot(metrics::MetricSample::available(AgentsReading {
            states: [
                AgentState::NotRunning,
                AgentState::RunningActivityUnknown,
                AgentState::NotRunning,
            ],
            age_ms: 1_200,
        }));
        assert_eq!(g.availability, Availability::Available);
        assert_eq!((g.state, g.source_age_ms), ([0, 1, 0], 1_200));
        let down = agents_snapshot(metrics::MetricSample::unavailable(
            MetricAvailability::TemporarilyUnavailable,
        ));
        assert_eq!(down.availability, Availability::TemporarilyUnavailable);
        assert_eq!((down.state, down.source_age_ms), ([0; 3], 0));
    }
}
