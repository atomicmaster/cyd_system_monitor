// SPDX-License-Identifier: GPL-3.0-only

//! Network: primary-route traffic and Host WiFi context (group key 9).

use metrics::network::{Coverage, NetworkCollector, NetworkReading, RouteKind as NetRouteKind};
use metrics::wifi_context::ContextAvailability;
use metrics::{Collector, MetricAvailability};
use protocol::{
    AssociationContext, Availability, NetworkSnapshot, RouteKind, Snapshot, WifiCoverage,
};

use super::{availability, Group, Report};

impl Group for NetworkCollector {
    fn collect_into(&mut self, now_ms: u64, snapshot: &mut Snapshot, report: &mut Report) {
        let network = network_snapshot(self.collect(now_ms).value);
        report.status("network", network.traffic_availability);
        report.line_with_provider_drops(format!(
            "network {:?} kind {:?}{} rate {} context {:?} coverage {:?} ch {} station {} ssid {} bssid {}",
            network.traffic_availability,
            network.route_kind,
            if network.via_tunnel { " via tunnel" } else { "" },
            if network.traffic_availability == Availability::Available {
                format!("{}/{} B/s", network.rx_bytes_per_s, network.tx_bytes_per_s)
            } else {
                "unavailable".to_string()
            },
            network.context_availability,
            network.coverage,
            network.channel,
            if network.station.is_some() { "present" } else { "absent" },
            if network.ssid.is_empty() { "absent" } else { "present" },
            if network.bssid.is_some() { "present" } else { "absent" },
        ));
        snapshot.network = Some(network);
    }

    fn wake(&mut self) {
        self.on_wake();
    }
}

/// The wire group for one second of network collection. Each half carries its
/// own availability; a rate that is not available is zero with its
/// availability (never a measured zero), and SSID/BSSID travel only with an
/// available context.
fn network_snapshot(reading: Option<NetworkReading>) -> NetworkSnapshot {
    let Some(reading) = reading else {
        return NetworkSnapshot {
            traffic_availability: Availability::Error,
            traffic_age_ms: 0,
            route_kind: RouteKind::Unknown,
            via_tunnel: false,
            rx_bytes_per_s: 0,
            tx_bytes_per_s: 0,
            context_availability: AssociationContext::Error,
            context_age_ms: 0,
            coverage: WifiCoverage::Unknown,
            channel: 0,
            station: None,
            ssid: Vec::new(),
            bssid: None,
        };
    };
    let (t, w) = (reading.traffic, reading.wifi);
    let snapshot = NetworkSnapshot {
        traffic_availability: match (t.availability, t.rate) {
            (MetricAvailability::Available, None) => Availability::Error, // no value behind it
            (other, _) => availability(other),
        },
        // The sample is taken as it is collected.
        traffic_age_ms: 0,
        route_kind: match t.route_kind {
            NetRouteKind::Unknown => RouteKind::Unknown,
            NetRouteKind::Wifi => RouteKind::Wifi,
            NetRouteKind::Ethernet => RouteKind::Ethernet,
            NetRouteKind::Other => RouteKind::Other,
            NetRouteKind::None => RouteKind::None,
        },
        via_tunnel: t.via_tunnel,
        rx_bytes_per_s: t.rate.map_or(0, |r| r.rx_bytes_per_s),
        tx_bytes_per_s: t.rate.map_or(0, |r| r.tx_bytes_per_s),
        context_availability: match w.availability {
            ContextAvailability::Available => AssociationContext::Available,
            ContextAvailability::PermissionNotRequested => {
                AssociationContext::PermissionNotRequested
            }
            ContextAvailability::Denied => AssociationContext::Denied,
            ContextAvailability::Restricted => AssociationContext::Restricted,
            ContextAvailability::AuthorizedUnavailable => AssociationContext::AuthorizedUnavailable,
            ContextAvailability::Unsupported => AssociationContext::Unsupported,
            ContextAvailability::Error => AssociationContext::Error,
        },
        context_age_ms: w.age_ms,
        coverage: match w.coverage {
            Coverage::Unknown => WifiCoverage::Unknown,
            Coverage::Observable => WifiCoverage::Observable,
            Coverage::OutsideSupported => WifiCoverage::OutsideSupported,
        },
        channel: w.channel,
        station: w.station,
        ssid: w.ssid,
        bssid: w.bssid,
    };
    if snapshot.is_valid() {
        snapshot
    } else {
        // Defensive: never send a group the codecs would refuse.
        NetworkSnapshot {
            traffic_availability: Availability::Error,
            rx_bytes_per_s: 0,
            tx_bytes_per_s: 0,
            via_tunnel: false,
            context_availability: AssociationContext::Error,
            coverage: WifiCoverage::Unknown,
            channel: 0,
            ssid: Vec::new(),
            bssid: None,
            ..snapshot
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn network_readings_map_to_valid_wire_groups() {
        use metrics::network::{NetworkRate, TrafficHalf, WifiHalf};
        let reading = |availability, rate, kind, via_tunnel, wifi| NetworkReading {
            traffic: TrafficHalf {
                availability,
                route_kind: kind,
                via_tunnel,
                rate,
            },
            wifi,
        };
        let no_wifi = |availability| WifiHalf {
            availability,
            age_ms: 0,
            coverage: Coverage::Unknown,
            channel: 0,
            station: Some([2, 0, 0, 0, 0, 1]),
            ssid: Vec::new(),
            bssid: None,
        };
        let rate = Some(NetworkRate {
            rx_bytes_per_s: 1200,
            tx_bytes_per_s: 300,
        });
        // No provider: traffic and station identity still flow.
        let g = network_snapshot(
            reading(
                MetricAvailability::Available,
                rate,
                NetRouteKind::Wifi,
                false,
                no_wifi(ContextAvailability::PermissionNotRequested),
            )
            .into(),
        );
        assert!(g.is_valid());
        assert_eq!((g.rx_bytes_per_s, g.tx_bytes_per_s), (1200, 300));
        assert_eq!(
            g.context_availability,
            AssociationContext::PermissionNotRequested
        );
        assert!(g.station.is_some());
        // A rate pending after a route change is unavailable with zero values.
        let g = network_snapshot(
            reading(
                MetricAvailability::TemporarilyUnavailable,
                None,
                NetRouteKind::Ethernet,
                false,
                no_wifi(ContextAvailability::Unsupported),
            )
            .into(),
        );
        assert_eq!(g.traffic_availability, Availability::TemporarilyUnavailable);
        assert_eq!((g.rx_bytes_per_s, g.route_kind), (0, RouteKind::Ethernet));
        // Offline is Absent with route kind none; an unreadable system is Error.
        let g = network_snapshot(
            reading(
                MetricAvailability::Absent,
                None,
                NetRouteKind::None,
                false,
                no_wifi(ContextAvailability::PermissionNotRequested),
            )
            .into(),
        );
        assert_eq!(
            (g.traffic_availability, g.route_kind),
            (Availability::Absent, RouteKind::None)
        );
        assert_eq!(
            network_snapshot(None).traffic_availability,
            Availability::Error
        );
        // An impossible reading is degraded, never sent as an invalid group.
        let g = network_snapshot(
            reading(
                MetricAvailability::Available,
                None,
                NetRouteKind::Wifi,
                false,
                no_wifi(ContextAvailability::PermissionNotRequested),
            )
            .into(),
        );
        assert!(g.is_valid());
        assert_eq!(g.traffic_availability, Availability::Error);
    }
}
