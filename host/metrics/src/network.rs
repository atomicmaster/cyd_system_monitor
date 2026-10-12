// SPDX-License-Identifier: GPL-3.0-only

//! Network (TB05): traffic totals over the active physical links and Host WiFi
//! coverage. Pure and fixture-tested; the macOS system view lives in
//! `platform-macos`.
//!
//! Rules (`docs/host-metrics.md`, `CONTEXT.md`):
//! - The rates are the totals over every active physical link
//!   ([`active_links`]): WiFi and Ethernet ports with a link, plus the primary
//!   route's own interface when it is some other non-tunnel link. Tunnels are
//!   never counted (their traffic crosses a physical link already); a tunnel
//!   primary route only sets `via_tunnel`.
//! - A total is unavailable (never zero) when any of its links has no
//!   comparable previous sample: the first sample, a link joining, a counter
//!   decrease, a non-monotonic clock, an over-long gap, or a change of
//!   interface or WiFi Host Station Identity (each re-baselines that link). A
//!   link leaving simply stops counting.
//! - Host Station Identity needs no permission; band/channel and SSID/BSSID
//!   come only from a [`WifiContextProvider`](crate::wifi_context).

use crate::wifi_context::{ContextAvailability, ContextReading, WifiBand, WifiContextProvider};
use crate::{Collector, CounterRateSampler, MetricAvailability, MetricSample};

/// What an interface is, as far as the Host can tell.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinkKind {
    Wifi,
    Ethernet,
    /// A physical or virtual link that is neither (Thunderbolt bridge, ...).
    Other,
    /// `utun`, `ipsec`, `ppp`, ...: a tunnel over some other interface.
    Tunnel,
    Loopback,
}

/// Wire values of the primary-route interface kind (`CORE_PROTOCOL.md`, key 9).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum RouteKind {
    Unknown = 0,
    Wifi = 1,
    Ethernet = 2,
    Other = 3,
    None = 4,
}

/// Cumulative 64-bit byte counters of one interface (`if_data64`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InterfaceCounters {
    pub rx_bytes: u64,
    pub tx_bytes: u64,
}

/// Bytes per second in each direction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NetworkRate {
    pub rx_bytes_per_s: u64,
    pub tx_bytes_per_s: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InterfaceInfo {
    pub name: String,
    pub index: u32,
    pub kind: LinkKind,
    /// `IFF_UP` and `IFF_RUNNING`.
    pub up_running: bool,
    /// A WiFi or Ethernet port with an active link (carrier or association):
    /// a physical link whose traffic the totals count. macOS keeps unplugged
    /// ports up and running, so `up_running` alone does not say this.
    pub link_active: bool,
    /// The current `AF_LINK` address, for a WiFi interface (Host Station
    /// Identity, which may be a private randomized address).
    pub station: Option<[u8; 6]>,
    pub counters: Option<InterfaceCounters>,
}

/// One default route from the routing table, in table order. `scoped` routes
/// (`RTF_IFSCOPE`) are per-interface fallbacks, not the system's choice.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DefaultRoute {
    pub interface: String,
    pub scoped: bool,
}

/// One second's view of the system's interfaces and default routes.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SystemView {
    pub interfaces: Vec<InterfaceInfo>,
    pub default_routes: Vec<DefaultRoute>,
}

/// Reads the system view. `None` when it cannot be read at all.
pub trait NetworkSystem {
    fn read(&mut self) -> Option<SystemView>;
    /// Drop anything cached between reads (after a sleep the hardware may
    /// have changed).
    fn invalidate(&mut self) {}
}

/// The interface the Monitored Host's primary route uses.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PrimaryRoute {
    pub interface: String,
    pub index: u32,
    /// The interface's own kind, or for a tunnel the kind of the link under it
    /// (`Other` when that cannot be told).
    pub kind: RouteKind,
    pub via_tunnel: bool,
    /// The routed interface's Host Station Identity when it is WiFi.
    pub station: Option<[u8; 6]>,
}

fn route_kind(kind: LinkKind) -> RouteKind {
    match kind {
        LinkKind::Wifi => RouteKind::Wifi,
        LinkKind::Ethernet => RouteKind::Ethernet,
        LinkKind::Other | LinkKind::Tunnel => RouteKind::Other,
        LinkKind::Loopback => RouteKind::Unknown,
    }
}

/// Picks the primary route: the first unscoped default route whose interface
/// is up and running (the routing table lists routes in service order), else
/// the first scoped one. `None` means offline. A tunnel keeps its own
/// interface; the underlying kind comes from the scoped (or first other)
/// default route beneath it, so the WiFi context still applies to a VPN over
/// WiFi. Tunnel and underlying traffic are never combined.
pub fn select_primary_route(view: &SystemView) -> Option<PrimaryRoute> {
    let usable = |route: &&DefaultRoute| {
        view.interfaces
            .iter()
            .any(|i| i.name == route.interface && i.up_running && i.kind != LinkKind::Loopback)
    };
    let interface_of = |route: &DefaultRoute| {
        view.interfaces
            .iter()
            .find(|i| i.name == route.interface)
            .expect("usable() checked")
    };
    let chosen = view
        .default_routes
        .iter()
        .filter(usable)
        .find(|r| !r.scoped)
        .or_else(|| view.default_routes.iter().find(usable))?;
    let primary = interface_of(chosen);
    if primary.kind != LinkKind::Tunnel {
        return Some(PrimaryRoute {
            interface: primary.name.clone(),
            index: primary.index,
            kind: route_kind(primary.kind),
            via_tunnel: false,
            station: primary.station,
        });
    }
    let underlying = view
        .default_routes
        .iter()
        .filter(usable)
        .map(interface_of)
        .find(|i| i.kind != LinkKind::Tunnel);
    Some(PrimaryRoute {
        interface: primary.name.clone(),
        index: primary.index,
        kind: underlying.map_or(RouteKind::Other, |i| route_kind(i.kind)),
        via_tunnel: true,
        station: None,
    })
}

/// Turns cumulative interface counters into a rate: the shared
/// [`CounterRateSampler`] discipline keyed by interface name, so the first
/// sample, a changed interface, a counter decrease (reset or interface
/// re-created), a non-monotonic clock, an over-long gap or zero elapsed time
/// are all unavailable, and every call re-baselines. An idle link is a
/// measured zero rate, not unavailable.
#[derive(Debug, Default)]
pub struct RateSampler(CounterRateSampler<String>);

impl RateSampler {
    pub fn sample(
        &mut self,
        key: &str,
        counters: InterfaceCounters,
        now_ms: u64,
    ) -> MetricSample<NetworkRate> {
        self.0
            .sample(
                key.to_string(),
                [counters.rx_bytes, counters.tx_bytes],
                now_ms,
            )
            .map(|[rx, tx]| NetworkRate {
                rx_bytes_per_s: rx,
                tx_bytes_per_s: tx,
            })
    }

    pub fn reset(&mut self) {
        self.0.reset();
    }
}

/// Host WiFi Coverage (`CONTEXT.md`, wire key 8 of the Network group).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Coverage {
    Unknown = 0,
    /// 2.4 GHz: the board can observe the band. Not a promise of continuous
    /// or current capture.
    Observable = 1,
    /// 5 or 6 GHz: the board cannot observe it.
    OutsideSupported = 2,
}

/// Maps a band and/or channel to coverage and the channel to report.
///
/// A known band decides. Without a band a channel number is only decisive
/// above 14: 2.4 GHz never exceeds 14, so 15-233 is 5 or 6 GHz and outside
/// coverage, while 1-14 could equally be 6 GHz (whose channels start at 1) and
/// stays unknown. A band and channel that contradict each other, a channel
/// outside every band's numbering, or nothing at all is unknown with channel 0
/// (never a guess).
pub fn coverage_for(band: Option<WifiBand>, channel: Option<u16>) -> (Coverage, u8) {
    let unknown = (Coverage::Unknown, 0);
    let channel = match channel {
        Some(0) | None => None,
        Some(c) if c > 233 => return unknown,
        Some(c) => Some(c),
    };
    match (band, channel) {
        (Some(WifiBand::Ghz2_4), None) => (Coverage::Observable, 0),
        (Some(WifiBand::Ghz2_4), Some(c)) if c <= 14 => (Coverage::Observable, c as u8),
        (Some(WifiBand::Ghz5), None) | (Some(WifiBand::Ghz6), None) => {
            (Coverage::OutsideSupported, 0)
        }
        // 5 GHz channel numbers run 32-177.
        (Some(WifiBand::Ghz5), Some(c)) if (32..=177).contains(&c) => {
            (Coverage::OutsideSupported, c as u8)
        }
        (Some(WifiBand::Ghz6), Some(c)) => (Coverage::OutsideSupported, c as u8),
        (None, Some(c)) if c > 14 => (Coverage::OutsideSupported, c as u8),
        _ => unknown,
    }
}

/// The physical links whose traffic the totals count, WiFi first, then
/// Ethernet, then others, each in interface order: every WiFi or Ethernet port
/// with an active link, plus the primary route's own interface when it is
/// some other link that is not a tunnel (a Thunderbolt bridge), so the route
/// is never left uncounted. At most [`MAX_LINKS`].
pub fn active_links<'a>(
    view: &'a SystemView,
    primary: Option<&PrimaryRoute>,
) -> Vec<&'a InterfaceInfo> {
    let mut links: Vec<&InterfaceInfo> = view
        .interfaces
        .iter()
        .filter(|i| {
            (i.link_active && matches!(i.kind, LinkKind::Wifi | LinkKind::Ethernet))
                || primary.is_some_and(|p| {
                    !p.via_tunnel && p.interface == i.name && i.kind == LinkKind::Other
                })
        })
        .collect();
    let rank = |kind: LinkKind| match kind {
        LinkKind::Wifi => 0,
        LinkKind::Ethernet => 1,
        _ => 2,
    };
    links.sort_by_key(|i| (rank(i.kind), i.index));
    links.truncate(MAX_LINKS);
    links
}

/// At most this many links are counted and reported (the wire bound).
pub const MAX_LINKS: usize = 8;

fn link_kind(kind: LinkKind) -> RouteKind {
    match kind {
        LinkKind::Wifi => RouteKind::Wifi,
        LinkKind::Ethernet => RouteKind::Ethernet,
        _ => RouteKind::Other,
    }
}

/// The traffic half of one second's reading.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrafficHalf {
    /// Available when `rate` is set; TemporarilyUnavailable while a baseline
    /// is being (re)built; Absent when offline; Error when unreadable.
    pub availability: MetricAvailability,
    pub route_kind: RouteKind,
    pub via_tunnel: bool,
    pub rate: Option<NetworkRate>,
    /// The kinds of the links the rate totals, one each ([`active_links`]).
    pub links: Vec<RouteKind>,
}

/// The WiFi half: Host Station Identity from the daemon itself plus whatever
/// the provider says about band/channel and SSID/BSSID.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WifiHalf {
    pub availability: ContextAvailability,
    pub age_ms: u32,
    pub coverage: Coverage,
    pub channel: u8,
    pub station: Option<[u8; 6]>,
    pub ssid: Vec<u8>,
    pub bssid: Option<[u8; 6]>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetworkReading {
    pub traffic: TrafficHalf,
    pub wifi: WifiHalf,
}

/// The Network collector: system view -> primary route -> rate, plus the
/// provider's context. It always returns a reading; each half carries its own
/// availability, and an absent provider never blocks the traffic half.
pub struct NetworkCollector {
    system: Box<dyn NetworkSystem>,
    provider: Box<dyn WifiContextProvider>,
    /// One rate baseline per link, keyed by interface, index and WiFi identity.
    samplers: std::collections::HashMap<String, RateSampler>,
}

impl NetworkCollector {
    pub fn new(system: Box<dyn NetworkSystem>, provider: Box<dyn WifiContextProvider>) -> Self {
        Self {
            system,
            provider,
            samplers: std::collections::HashMap::new(),
        }
    }

    fn traffic(&mut self, view: Option<&SystemView>, now_ms: u64) -> TrafficHalf {
        let Some(view) = view else {
            self.samplers.clear();
            return TrafficHalf {
                availability: MetricAvailability::Error,
                route_kind: RouteKind::Unknown,
                via_tunnel: false,
                rate: None,
                links: Vec::new(),
            };
        };
        let route = select_primary_route(view);
        let links = active_links(view, route.as_ref());
        let kinds = links.iter().map(|i| link_kind(i.kind)).collect();
        let Some(route) = route else {
            self.samplers.clear();
            return TrafficHalf {
                availability: MetricAvailability::Absent,
                route_kind: RouteKind::None,
                via_tunnel: false,
                rate: None,
                links: kinds,
            };
        };
        // Every link is sampled each second, so each keeps its own baseline;
        // links that left are forgotten.
        let mut total = Some(NetworkRate {
            rx_bytes_per_s: 0,
            tx_bytes_per_s: 0,
        });
        let mut availability = MetricAvailability::Available;
        let mut seen = std::collections::HashSet::new();
        for link in &links {
            let key = format!(
                "{}#{}#{}",
                link.name,
                link.index,
                link.station
                    .map_or(String::new(), |m| m.map(|b| format!("{b:02x}")).concat())
            );
            seen.insert(link.name.clone());
            let Some(counters) = link.counters else {
                availability = MetricAvailability::Error;
                total = None;
                continue;
            };
            let sample = self
                .samplers
                .entry(link.name.clone())
                .or_default()
                .sample(&key, counters, now_ms);
            match (sample.value, total) {
                (Some(rate), Some(sum)) => {
                    total = Some(NetworkRate {
                        rx_bytes_per_s: sum.rx_bytes_per_s.saturating_add(rate.rx_bytes_per_s),
                        tx_bytes_per_s: sum.tx_bytes_per_s.saturating_add(rate.tx_bytes_per_s),
                    });
                }
                (None, _) => {
                    if availability != MetricAvailability::Error {
                        availability = sample.availability;
                    }
                    total = None;
                }
                (Some(_), None) => {}
            }
        }
        self.samplers.retain(|name, _| seen.contains(name));
        if links.is_empty() {
            // A route with no link to count (cannot normally happen).
            availability = MetricAvailability::Error;
            total = None;
        }
        TrafficHalf {
            availability,
            route_kind: route.kind,
            via_tunnel: route.via_tunnel,
            rate: total,
            links: kinds,
        }
    }

    fn wifi(&mut self, view: Option<&SystemView>, now_ms: u64) -> WifiHalf {
        let station = view.and_then(|v| {
            v.interfaces
                .iter()
                .filter(|i| i.kind == LinkKind::Wifi && i.up_running)
                .find_map(|i| i.station)
        });
        let wifi_active = view.is_some_and(|v| {
            v.interfaces
                .iter()
                .any(|i| i.kind == LinkKind::Wifi && i.up_running)
        });
        let ContextReading { context, age_ms } = self.provider.latest(now_ms);
        // A provider cannot describe an association the Host does not have.
        if !wifi_active {
            return WifiHalf {
                availability: context.availability,
                age_ms: 0,
                coverage: Coverage::Unknown,
                channel: 0,
                station: None,
                ssid: Vec::new(),
                bssid: None,
            };
        }
        let (coverage, channel) = coverage_for(context.band, context.channel);
        let available = context.availability == ContextAvailability::Available;
        WifiHalf {
            availability: context.availability,
            age_ms,
            coverage,
            channel,
            station,
            ssid: if available {
                context.ssid.unwrap_or_default()
            } else {
                Vec::new()
            },
            bssid: if available { context.bssid } else { None },
        }
    }
}

impl Collector for NetworkCollector {
    type Reading = NetworkReading;
    fn collect(&mut self, now_ms: u64) -> MetricSample<NetworkReading> {
        let view = self.system.read();
        let traffic = self.traffic(view.as_ref(), now_ms);
        let wifi = self.wifi(view.as_ref(), now_ms);
        MetricSample::available(NetworkReading { traffic, wifi })
    }

    /// The provider's context needs nothing: it is aged by the same clock and
    /// dropped past 15 s.
    fn on_wake(&mut self) {
        self.samplers.clear();
        self.system.invalidate();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wifi_context::{AssociationContext, FixtureProvider};
    use crate::MAX_RATE_SAMPLE_GAP_MS;

    fn counters(rx: u64, tx: u64) -> InterfaceCounters {
        InterfaceCounters {
            rx_bytes: rx,
            tx_bytes: tx,
        }
    }

    fn iface(name: &str, index: u32, kind: LinkKind, rx: u64, tx: u64) -> InterfaceInfo {
        InterfaceInfo {
            name: name.into(),
            index,
            kind,
            up_running: true,
            link_active: matches!(kind, LinkKind::Wifi | LinkKind::Ethernet),
            station: (kind == LinkKind::Wifi).then_some([2, 0, 0, 0, 0, index as u8]),
            counters: Some(counters(rx, tx)),
        }
    }

    fn route(name: &str, scoped: bool) -> DefaultRoute {
        DefaultRoute {
            interface: name.into(),
            scoped,
        }
    }

    #[test]
    fn wifi_is_the_primary_route_when_it_holds_the_unscoped_default() {
        let view = SystemView {
            interfaces: vec![
                iface("lo0", 1, LinkKind::Loopback, 0, 0),
                iface("en0", 6, LinkKind::Wifi, 0, 0),
                iface("en5", 7, LinkKind::Ethernet, 0, 0),
            ],
            default_routes: vec![route("en0", false), route("en5", true)],
        };
        let p = select_primary_route(&view).unwrap();
        assert_eq!(
            (p.interface.as_str(), p.kind, p.via_tunnel),
            ("en0", RouteKind::Wifi, false)
        );
        assert_eq!(p.station, Some([2, 0, 0, 0, 0, 6]));
    }

    #[test]
    fn a_tunnel_primary_route_keeps_its_own_interface_and_names_the_link_under_it() {
        let view = SystemView {
            interfaces: vec![
                iface("utun4", 20, LinkKind::Tunnel, 0, 0),
                iface("en0", 6, LinkKind::Wifi, 0, 0),
            ],
            default_routes: vec![route("utun4", false), route("en0", true)],
        };
        let p = select_primary_route(&view).unwrap();
        assert_eq!(p.interface, "utun4");
        assert!(p.via_tunnel);
        assert_eq!(p.kind, RouteKind::Wifi);
        // No physical link under it that we can name: Other, never Unknown.
        let alone = SystemView {
            interfaces: vec![iface("utun4", 20, LinkKind::Tunnel, 0, 0)],
            default_routes: vec![route("utun4", false)],
        };
        let p = select_primary_route(&alone).unwrap();
        assert_eq!((p.kind, p.via_tunnel), (RouteKind::Other, true));
    }

    #[test]
    fn no_usable_default_route_is_offline() {
        let mut down = iface("en0", 6, LinkKind::Wifi, 0, 0);
        down.up_running = false;
        let view = SystemView {
            interfaces: vec![down, iface("lo0", 1, LinkKind::Loopback, 0, 0)],
            default_routes: vec![
                route("en0", false),
                route("lo0", false),
                route("gone9", false),
            ],
        };
        assert_eq!(select_primary_route(&view), None);
        assert_eq!(select_primary_route(&SystemView::default()), None);
    }

    #[test]
    fn a_scoped_default_is_used_only_when_there_is_no_unscoped_one() {
        let view = SystemView {
            interfaces: vec![
                iface("en0", 6, LinkKind::Wifi, 0, 0),
                iface("en5", 7, LinkKind::Ethernet, 0, 0),
            ],
            default_routes: vec![route("en0", true), route("en5", true)],
        };
        assert_eq!(select_primary_route(&view).unwrap().interface, "en0");
    }

    #[test]
    fn rate_needs_a_baseline_and_an_idle_link_is_a_measured_zero() {
        let mut s = RateSampler::default();
        let first = s.sample("en0", counters(1000, 1000), 0);
        assert_eq!(
            first.availability,
            MetricAvailability::TemporarilyUnavailable
        );
        assert_eq!(first.value, None);
        let r = s
            .sample("en0", counters(1_201_000, 341_000), 1000)
            .value
            .unwrap();
        assert_eq!((r.rx_bytes_per_s, r.tx_bytes_per_s), (1_200_000, 340_000));
        // A late tick scales by the real elapsed time.
        let r = s
            .sample("en0", counters(1_204_000, 341_000), 3000)
            .value
            .unwrap();
        assert_eq!((r.rx_bytes_per_s, r.tx_bytes_per_s), (1_500, 0));
        let idle = s.sample("en0", counters(1_204_000, 341_000), 4000);
        assert_eq!(idle.availability, MetricAvailability::Available);
    }

    #[test]
    fn a_new_key_a_counter_reset_or_a_bad_clock_rebaselines() {
        let mut s = RateSampler::default();
        s.sample("en0", counters(1_000_000, 1_000_000), 0);
        // Route/interface change: a different key is unavailable, then works.
        assert_eq!(s.sample("utun4", counters(5, 5), 1000).value, None);
        assert!(s.sample("utun4", counters(105, 205), 2000).value.is_some());
        // A counter decrease in either direction.
        assert_eq!(s.sample("utun4", counters(1, 300), 3000).value, None);
        assert_eq!(s.sample("utun4", counters(100, 1), 4000).value, None);
        assert!(s.sample("utun4", counters(200, 101), 5000).value.is_some());
        // Over-long gap (sleep/wake), backwards clock, zero elapsed time.
        assert_eq!(
            s.sample(
                "utun4",
                counters(300, 201),
                5000 + MAX_RATE_SAMPLE_GAP_MS + 1
            )
            .value,
            None
        );
        let t = 5000 + MAX_RATE_SAMPLE_GAP_MS + 1;
        assert_eq!(s.sample("utun4", counters(400, 301), t - 5).value, None);
        assert_eq!(s.sample("utun4", counters(500, 401), t - 5).value, None);
        // A full 64-bit delta does not overflow.
        s.reset();
        s.sample("x", counters(0, 0), 0);
        let r = s
            .sample("x", counters(u64::MAX, u64::MAX), 1000)
            .value
            .unwrap();
        assert_eq!(r.rx_bytes_per_s, u64::MAX);
    }

    #[test]
    fn coverage_follows_band_and_never_guesses_from_an_ambiguous_channel() {
        use WifiBand::*;
        assert_eq!(
            coverage_for(Some(Ghz2_4), Some(6)),
            (Coverage::Observable, 6)
        );
        assert_eq!(coverage_for(Some(Ghz2_4), None), (Coverage::Observable, 0));
        assert_eq!(
            coverage_for(Some(Ghz5), Some(149)),
            (Coverage::OutsideSupported, 149)
        );
        assert_eq!(
            coverage_for(Some(Ghz6), Some(5)),
            (Coverage::OutsideSupported, 5)
        );
        assert_eq!(
            coverage_for(Some(Ghz5), None),
            (Coverage::OutsideSupported, 0)
        );
        // No band: above 14 is certainly 5/6 GHz; 1-14 could be 6 GHz too.
        assert_eq!(
            coverage_for(None, Some(36)),
            (Coverage::OutsideSupported, 36)
        );
        assert_eq!(coverage_for(None, Some(11)), (Coverage::Unknown, 0));
        assert_eq!(coverage_for(None, None), (Coverage::Unknown, 0));
        assert_eq!(coverage_for(None, Some(0)), (Coverage::Unknown, 0));
        // Contradictions and impossible numbers are unknown.
        assert_eq!(coverage_for(Some(Ghz2_4), Some(36)), (Coverage::Unknown, 0));
        assert_eq!(coverage_for(Some(Ghz5), Some(6)), (Coverage::Unknown, 0));
        assert_eq!(coverage_for(Some(Ghz6), Some(234)), (Coverage::Unknown, 0));
        assert_eq!(coverage_for(None, Some(65535)), (Coverage::Unknown, 0));
    }

    fn collector(views: Vec<Option<SystemView>>, provider: FixtureProvider) -> NetworkCollector {
        struct Script(Vec<Option<SystemView>>, usize);
        impl NetworkSystem for Script {
            fn read(&mut self) -> Option<SystemView> {
                let v = self.0[self.1.min(self.0.len() - 1)].clone();
                self.1 += 1;
                v
            }
        }
        NetworkCollector::new(Box::new(Script(views, 0)), Box::new(provider))
    }

    fn wifi_view(rx: u64, tx: u64) -> SystemView {
        SystemView {
            interfaces: vec![iface("en0", 6, LinkKind::Wifi, rx, tx)],
            default_routes: vec![route("en0", false)],
        }
    }

    #[test]
    fn without_a_provider_traffic_and_station_identity_are_still_reported() {
        let mut c = collector(
            vec![Some(wifi_view(0, 0)), Some(wifi_view(1_000, 500))],
            FixtureProvider::absent(),
        );
        let first = c.collect(0).value.unwrap();
        assert_eq!(
            first.traffic.availability,
            MetricAvailability::TemporarilyUnavailable
        );
        assert_eq!(first.traffic.rate, None);
        let second = c.collect(1000).value.unwrap();
        assert_eq!(second.traffic.rate.unwrap().rx_bytes_per_s, 1_000);
        assert_eq!(second.traffic.route_kind, RouteKind::Wifi);
        assert_eq!(
            second.wifi.availability,
            ContextAvailability::PermissionNotRequested
        );
        assert_eq!(second.wifi.coverage, Coverage::Unknown);
        assert_eq!(second.wifi.station, Some([2, 0, 0, 0, 0, 6]));
        assert!(second.wifi.ssid.is_empty() && second.wifi.bssid.is_none());
    }

    #[test]
    fn a_wake_forgets_the_rate_baseline_and_the_system_view_cache() {
        use std::sync::atomic::{AtomicU32, Ordering};
        use std::sync::Arc;
        struct Counting(u64, Arc<AtomicU32>);
        impl NetworkSystem for Counting {
            fn read(&mut self) -> Option<SystemView> {
                self.0 += 1_000;
                Some(wifi_view(self.0, 0))
            }
            fn invalidate(&mut self) {
                self.1.fetch_add(1, Ordering::SeqCst);
            }
        }
        let invalidated = Arc::new(AtomicU32::new(0));
        let mut c = NetworkCollector::new(
            Box::new(Counting(0, invalidated.clone())),
            Box::new(FixtureProvider::absent()),
        );
        c.collect(0);
        assert!(c.collect(1000).value.unwrap().traffic.rate.is_some());
        // Even a wake that the rate interval alone would not reveal is never
        // averaged across: the first post-wake sample is a new baseline.
        c.on_wake();
        assert_eq!(invalidated.load(Ordering::SeqCst), 1);
        let woke = c.collect(2000).value.unwrap().traffic;
        assert_eq!(
            (woke.availability, woke.rate),
            (MetricAvailability::TemporarilyUnavailable, None)
        );
        assert!(c.collect(3000).value.unwrap().traffic.rate.is_some());
    }

    #[test]
    fn a_route_change_makes_the_rate_unavailable_then_it_recovers() {
        let eth = |rx, tx| SystemView {
            interfaces: vec![
                iface("en0", 6, LinkKind::Wifi, 10, 10),
                iface("en5", 7, LinkKind::Ethernet, rx, tx),
            ],
            default_routes: vec![route("en5", false), route("en0", true)],
        };
        let mut c = collector(
            vec![
                Some(wifi_view(0, 0)),
                Some(wifi_view(1_000, 0)),
                Some(eth(9_000, 9_000)), // route moved to Ethernet
                Some(eth(9_500, 9_100)),
            ],
            FixtureProvider::absent(),
        );
        c.collect(0);
        assert!(c.collect(1000).value.unwrap().traffic.rate.is_some());
        let moved = c.collect(2000).value.unwrap().traffic;
        assert_eq!(
            (moved.availability, moved.rate, moved.route_kind),
            (
                MetricAvailability::TemporarilyUnavailable,
                None,
                RouteKind::Ethernet
            )
        );
        let back = c.collect(3000).value.unwrap().traffic;
        assert_eq!(back.rate.unwrap().rx_bytes_per_s, 500);
    }

    #[test]
    fn a_wifi_identity_change_rebaselines_the_rate() {
        let mut roamed = wifi_view(5_000, 0);
        roamed.interfaces[0].station = Some([2, 0, 0, 0, 0, 99]);
        let mut c = collector(
            vec![
                Some(wifi_view(0, 0)),
                Some(wifi_view(1_000, 0)),
                Some(roamed),
            ],
            FixtureProvider::absent(),
        );
        c.collect(0);
        assert!(c.collect(1000).value.unwrap().traffic.rate.is_some());
        assert_eq!(c.collect(2000).value.unwrap().traffic.rate, None);
    }

    #[test]
    fn a_tunnel_is_never_counted_on_top_of_the_link_under_it() {
        let view = |tun: u64, en0: u64| SystemView {
            interfaces: vec![
                iface("utun4", 20, LinkKind::Tunnel, tun, tun),
                iface("en0", 6, LinkKind::Wifi, en0, en0),
            ],
            default_routes: vec![route("utun4", false), route("en0", true)],
        };
        let mut c = collector(
            vec![Some(view(0, 0)), Some(view(1_000, 1_400))],
            FixtureProvider::absent(),
        );
        c.collect(0);
        let t = c.collect(1000).value.unwrap().traffic;
        assert!(t.via_tunnel);
        assert_eq!(t.route_kind, RouteKind::Wifi);
        // The physical link's total, which already carries the tunnel: not 2_400.
        assert_eq!(t.rate.unwrap().rx_bytes_per_s, 1_400);
        assert_eq!(t.links, vec![RouteKind::Wifi]);
    }

    #[test]
    fn the_totals_cover_every_active_physical_link() {
        let view = |wifi: u64, eth: u64| SystemView {
            interfaces: vec![
                iface("en0", 6, LinkKind::Wifi, wifi, wifi / 2),
                iface("en5", 7, LinkKind::Ethernet, eth, eth / 4),
            ],
            default_routes: vec![route("en5", false), route("en0", true)],
        };
        let mut c = collector(
            vec![Some(view(0, 0)), Some(view(1_000, 8_000))],
            FixtureProvider::absent(),
        );
        assert_eq!(c.collect(0).value.unwrap().traffic.links.len(), 2);
        let t = c.collect(1000).value.unwrap().traffic;
        assert_eq!(t.route_kind, RouteKind::Ethernet);
        assert_eq!(
            t.rate.unwrap(),
            NetworkRate {
                rx_bytes_per_s: 9_000,
                tx_bytes_per_s: 2_500
            }
        );
        // One per link, WiFi first.
        assert_eq!(t.links, vec![RouteKind::Wifi, RouteKind::Ethernet]);
    }

    #[test]
    fn unplugged_ports_and_virtual_interfaces_are_not_links() {
        let mut unplugged = iface("en1", 10, LinkKind::Ethernet, 0, 0);
        unplugged.link_active = false;
        let view = |n: u64| SystemView {
            interfaces: vec![
                iface("en0", 6, LinkKind::Wifi, n, n),
                unplugged.clone(),
                iface("awdl0", 16, LinkKind::Other, n * 7, n * 7),
                iface("bridge0", 15, LinkKind::Other, n * 5, n * 5),
                iface("utun0", 18, LinkKind::Tunnel, n * 3, n * 3),
                iface("lo0", 1, LinkKind::Loopback, n * 9, n * 9),
            ],
            default_routes: vec![route("en0", false), route("utun0", true)],
        };
        let mut c = collector(
            vec![Some(view(0)), Some(view(1_000))],
            FixtureProvider::absent(),
        );
        c.collect(0);
        let t = c.collect(1000).value.unwrap().traffic;
        assert_eq!(t.links, vec![RouteKind::Wifi]);
        assert_eq!(t.rate.unwrap().rx_bytes_per_s, 1_000);
    }

    #[test]
    fn a_primary_route_over_another_kind_of_link_is_still_counted() {
        let view = |n: u64| SystemView {
            interfaces: vec![iface("bridge0", 15, LinkKind::Other, n, n)],
            default_routes: vec![route("bridge0", false)],
        };
        let mut c = collector(
            vec![Some(view(0)), Some(view(2_000))],
            FixtureProvider::absent(),
        );
        c.collect(0);
        let t = c.collect(1000).value.unwrap().traffic;
        assert_eq!(t.links, vec![RouteKind::Other]);
        assert_eq!(t.rate.unwrap().rx_bytes_per_s, 2_000);
    }

    #[test]
    fn a_joining_link_makes_the_total_unavailable_once_and_a_leaving_one_does_not() {
        let one = |n: u64| SystemView {
            interfaces: vec![iface("en0", 6, LinkKind::Wifi, n, n)],
            default_routes: vec![route("en0", false)],
        };
        let two = |n: u64, m: u64| SystemView {
            interfaces: vec![
                iface("en0", 6, LinkKind::Wifi, n, n),
                iface("en5", 7, LinkKind::Ethernet, m, m),
            ],
            default_routes: vec![route("en0", false)],
        };
        let mut c = collector(
            vec![
                Some(one(0)),
                Some(one(1_000)),
                Some(two(2_000, 50_000)), // Ethernet plugged in
                Some(two(3_000, 51_000)),
                Some(one(4_000)), // and unplugged
                Some(one(5_000)),
            ],
            FixtureProvider::absent(),
        );
        c.collect(0);
        assert!(c.collect(1000).value.unwrap().traffic.rate.is_some());
        let joined = c.collect(2000).value.unwrap().traffic;
        assert_eq!(
            (joined.availability, joined.rate),
            (MetricAvailability::TemporarilyUnavailable, None)
        );
        assert_eq!(
            c.collect(3000)
                .value
                .unwrap()
                .traffic
                .rate
                .unwrap()
                .rx_bytes_per_s,
            2_000
        );
        // The remaining link's total is still a true total.
        let left = c.collect(4000).value.unwrap().traffic;
        assert_eq!(left.rate.unwrap().rx_bytes_per_s, 1_000);
        assert_eq!(left.links, vec![RouteKind::Wifi]);
    }

    #[test]
    fn with_no_route_the_active_links_are_still_reported() {
        let view = SystemView {
            interfaces: vec![iface("en0", 6, LinkKind::Wifi, 0, 0)],
            default_routes: vec![],
        };
        let mut c = collector(vec![Some(view)], FixtureProvider::absent());
        let t = c.collect(0).value.unwrap().traffic;
        assert_eq!(t.availability, MetricAvailability::Absent);
        assert_eq!(t.route_kind, RouteKind::None);
        assert_eq!(t.links, vec![RouteKind::Wifi]);
    }

    #[test]
    fn offline_and_unreadable_are_distinct_and_never_zero() {
        let mut c = collector(
            vec![Some(SystemView::default()), None],
            FixtureProvider::absent(),
        );
        let offline = c.collect(0).value.unwrap();
        assert_eq!(offline.traffic.availability, MetricAvailability::Absent);
        assert_eq!(offline.traffic.route_kind, RouteKind::None);
        assert_eq!(offline.traffic.rate, None);
        let broken = c.collect(1000).value.unwrap();
        assert_eq!(broken.traffic.availability, MetricAvailability::Error);
        assert_eq!(broken.wifi.station, None);
    }

    #[test]
    fn provider_context_is_mapped_and_only_ssid_is_gated_by_availability() {
        let ctx = |availability, ssid: Option<&[u8]>| AssociationContext {
            availability,
            band: Some(WifiBand::Ghz5),
            channel: Some(149),
            ssid: ssid.map(<[u8]>::to_vec),
            bssid: ssid.map(|_| [1, 2, 3, 4, 5, 6]),
        };
        let mut c = collector(
            vec![Some(wifi_view(0, 0))],
            FixtureProvider::new(vec![
                ContextReading {
                    context: ctx(ContextAvailability::Denied, Some(b"leak")),
                    age_ms: 40,
                },
                ContextReading {
                    context: ctx(ContextAvailability::Available, Some(b"CYD")),
                    age_ms: 40,
                },
            ]),
        );
        let denied = c.collect(0).value.unwrap().wifi;
        // Coverage is still known without Location permission; SSID never is.
        assert_eq!(denied.coverage, Coverage::OutsideSupported);
        assert_eq!((denied.channel, denied.age_ms), (149, 40));
        assert!(denied.ssid.is_empty() && denied.bssid.is_none());
        let ok = c.collect(1000).value.unwrap().wifi;
        assert_eq!(ok.ssid, b"CYD");
        assert_eq!(ok.bssid, Some([1, 2, 3, 4, 5, 6]));
    }

    #[test]
    fn a_provider_cannot_describe_wifi_when_the_host_has_none_active() {
        let ethernet_only = SystemView {
            interfaces: vec![iface("en5", 7, LinkKind::Ethernet, 0, 0)],
            default_routes: vec![route("en5", false)],
        };
        let ctx = AssociationContext {
            availability: ContextAvailability::Available,
            band: Some(WifiBand::Ghz2_4),
            channel: Some(6),
            ssid: Some(b"stale".to_vec()),
            bssid: Some([9; 6]),
        };
        let mut c = collector(
            vec![Some(ethernet_only)],
            FixtureProvider::new(vec![ContextReading {
                context: ctx,
                age_ms: 0,
            }]),
        );
        let w = c.collect(0).value.unwrap().wifi;
        assert_eq!(
            (w.coverage, w.channel, w.station),
            (Coverage::Unknown, 0, None)
        );
        assert!(w.ssid.is_empty() && w.bssid.is_none());
    }
}
