// SPDX-License-Identifier: GPL-3.0-only

//! Host session over the board's serial port, shared by the foreground CLI
//! and the per-user daemon: handshake, pairing, one-second complete snapshots,
//! and the unpair / calibration-reset commands. The byte channel is a trait so
//! the daemon's integration tests can drive a scripted device.

use std::io;
use std::thread::sleep;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use metrics::network::{Coverage, NetworkCollector, NetworkReading, RouteKind as NetRouteKind};
use metrics::wifi_context::{ContextAvailability, WifiContextProvider};
use metrics::{Collector, MetricAvailability};
use platform_macos::agents::{AgentsCollector, AgentsReading};
use platform_macos::battery::{BatteryCollector, BatteryReading};
use platform_macos::cpu::CpuCollector;
use platform_macos::ram::RamCollector;
use platform_macos::storage::{StorageCollector, StorageReading};
use protocol::{
    AgentsSnapshot, AssociationContext, Availability, BatterySnapshot, CommandOperation,
    CommandResult, CpuSnapshot, Freshness, NetworkSnapshot, RamSnapshot, RouteKind,
    StorageSnapshot, WifiCoverage,
};
use transport::client::{Event, HostClient, Phase, Refusal};
use transport::serial::SerialPort;

use crate::identity::{self, Identity};

/// Opening the port resets the ESP32; give it time to boot to the console.
pub const BOOT_WAIT: Duration = Duration::from_millis(3_000);
const HELLO_RETRY: Duration = Duration::from_millis(1_500);
/// Longer than the board's own touch-confirmation timeout.
const HANDSHAKE_DEADLINE: Duration = Duration::from_secs(75);
const SNAPSHOT_PERIOD: Duration = Duration::from_secs(1);
/// A gap this much longer than the snapshot period between two ticks means the
/// Mac slept: the link is resynchronized with a fresh session.
const WAKE_GAP_MS: u64 = 5_000;

/// The byte channel to the display: a serial port, or a scripted device.
pub trait Channel: Send {
    /// Reads up to `buf.len()` bytes, waiting at most `timeout`; 0 on timeout.
    fn read_timeout(&mut self, buf: &mut [u8], timeout: Duration) -> io::Result<usize>;
    fn write_all(&mut self, bytes: &[u8]) -> io::Result<()>;
}

impl Channel for SerialPort {
    fn read_timeout(&mut self, buf: &mut [u8], timeout: Duration) -> io::Result<usize> {
        SerialPort::read_timeout(self, buf, timeout)
    }
    fn write_all(&mut self, bytes: &[u8]) -> io::Result<()> {
        SerialPort::write_all(self, bytes)
    }
}

#[derive(Debug)]
pub enum LinkError {
    Io(io::Error),
    Refused(Refusal),
    Timeout(&'static str),
    /// The caller asked to stop while connecting or serving.
    Cancelled,
}

impl From<io::Error> for LinkError {
    fn from(error: io::Error) -> Self {
        LinkError::Io(error)
    }
}

impl std::fmt::Display for LinkError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LinkError::Io(e) => write!(f, "serial error: {e}"),
            LinkError::Refused(Refusal::Incompatible) => {
                write!(f, "the display speaks an incompatible protocol version")
            }
            LinkError::Refused(Refusal::NotPairedWithYou) => write!(
                f,
                "the display is paired with another Mac; unpair it from that Mac, or send \
                 DEV:UNPAIR on its console"
            ),
            LinkError::Refused(Refusal::Declined) => {
                write!(f, "pairing was declined or timed out on the display")
            }
            LinkError::Refused(other) => write!(f, "the display refused the request ({other:?})"),
            LinkError::Timeout(what) => write!(f, "timed out {what}"),
            LinkError::Cancelled => write!(f, "stopped"),
        }
    }
}

pub fn utc_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn matching_code() -> io::Result<u32> {
    let bytes: [u8; 4] = identity::random_bytes()?;
    Ok(100_000 + u32::from_le_bytes(bytes) % 900_000)
}

/// What a connection attempt reports while it runs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Progress {
    Opened,
    Negotiated {
        major: u8,
        minor: u8,
    },
    /// First pairing: the operator must confirm this code on the display.
    AwaitingConfirmation {
        code: u32,
    },
    Paired,
}

/// Knobs and callbacks for [`Link::connect_with`].
pub struct ConnectOptions<'a> {
    pub boot_wait: Duration,
    pub deadline: Duration,
    /// Polled about every 100 ms; `true` abandons the attempt.
    pub cancel: &'a mut dyn FnMut() -> bool,
    pub on_progress: &'a mut dyn FnMut(Progress),
    /// Persists the identity after a first pairing.
    pub save_identity: &'a mut dyn FnMut(&Identity) -> io::Result<()>,
}

/// One serial connection with a completed handshake.
pub struct Link {
    port: Box<dyn Channel>,
    client: HostClient,
}

impl Link {
    /// Opens the port, negotiates, and pairs (asking the operator to confirm
    /// on the display the first time). Persists the device identity. The
    /// foreground CLI's entry point: it prints its progress.
    pub fn connect(port_path: &str, identity: &mut Identity) -> Result<Link, LinkError> {
        let port = SerialPort::open(port_path)?;
        println!("Opened {port_path}; waiting for the board to boot...");
        let mut cancel = || false;
        let mut progress = |p: Progress| match p {
            Progress::Opened | Progress::Paired => {
                if p == Progress::Paired {
                    println!("Paired. Sending live data.");
                }
            }
            Progress::Negotiated { major, minor } => {
                println!("Display speaks protocol {major}.{minor}.");
            }
            Progress::AwaitingConfirmation { code } => {
                println!("Waiting for the display. If it asks, confirm code {code:06}.");
            }
        };
        Link::connect_with(
            Box::new(port),
            identity,
            ConnectOptions {
                boot_wait: BOOT_WAIT,
                deadline: HANDSHAKE_DEADLINE,
                cancel: &mut cancel,
                on_progress: &mut progress,
                save_identity: &mut |id| identity::save(id),
            },
        )
    }

    /// The handshake over any [`Channel`]. `identity.device` decides whether
    /// this reconnects to a known display or starts a first pairing.
    pub fn connect_with(
        mut port: Box<dyn Channel>,
        identity: &mut Identity,
        options: ConnectOptions<'_>,
    ) -> Result<Link, LinkError> {
        let ConnectOptions {
            boot_wait,
            deadline,
            cancel,
            on_progress,
            save_identity,
        } = options;
        on_progress(Progress::Opened);
        sleep_cancellable(boot_wait, cancel)?;

        let code = matching_code()?;
        let session = identity::random_bytes()?;
        let mut client = HostClient::new(identity.host, identity.device, code, session);
        let started = Instant::now();
        let mut last_hello: Option<Instant> = None;
        let mut announced = false;
        let mut buf = [0u8; 512];

        while started.elapsed() < deadline {
            if cancel() {
                return Err(LinkError::Cancelled);
            }
            if client.phase() != Phase::PairSent
                && last_hello.is_none_or(|t| t.elapsed() >= HELLO_RETRY)
            {
                // Idempotent: harmless if the board is already in link mode.
                port.write_all(b"CYD:LINK\n")?;
                sleep(Duration::from_millis(150));
                client.set_local_offset_minutes(platform_macos::time::local_offset_minutes());
                port.write_all(&client.start(Some(utc_ms())).map_err(protocol_error)?)?;
                last_hello = Some(Instant::now());
            }
            let n = port.read_timeout(&mut buf, Duration::from_millis(100))?;
            let out = client.receive(&buf[..n]);
            for frame in &out.frames {
                port.write_all(frame)?;
            }
            for event in out.events {
                match event {
                    Event::Negotiated {
                        device_major,
                        device_minor,
                    } => on_progress(Progress::Negotiated {
                        major: device_major,
                        minor: device_minor,
                    }),
                    Event::Refused(reason) => return Err(LinkError::Refused(reason)),
                    Event::Paired { device_identity } => {
                        if identity.device != Some(device_identity) {
                            identity.device = Some(device_identity);
                            save_identity(identity)?;
                        }
                        on_progress(Progress::Paired);
                        return Ok(Link { port, client });
                    }
                    Event::CommandReply { .. } => {}
                }
            }
            if client.phase() == Phase::PairSent && !announced {
                announced = true;
                on_progress(Progress::AwaitingConfirmation { code });
            }
        }
        Err(LinkError::Timeout("waiting for the display to pair"))
    }

    /// Sends one complete snapshot per second until the link fails, the
    /// observer asks to stop, or a sleep/wake gap requires a fresh session.
    pub fn serve(
        &mut self,
        collectors: &mut Collectors,
        clock: &dyn Fn() -> u64,
        wall: &dyn Fn() -> u64,
        observer: &mut dyn ServeObserver,
    ) -> Result<ServeEnd, LinkError> {
        let mut next = Instant::now();
        let mut last_wall = wall();
        let mut buf = [0u8; 512];
        loop {
            let wall_now = wall();
            if wall_now.saturating_sub(last_wall) > SNAPSHOT_PERIOD.as_millis() as u64 + WAKE_GAP_MS
            {
                // The Mac slept: whatever the display saw is stale and the
                // USB link may have been reset. Start over with a new session.
                return Ok(ServeEnd::Resync);
            }
            last_wall = wall_now;
            let collected = collectors.collect(clock());
            self.client
                .set_local_offset_minutes(platform_macos::time::local_offset_minutes());
            let frame = self
                .client
                .snapshot(
                    &collected.cpu,
                    Some(&collected.ram),
                    collected.storage.as_ref(),
                    collected.battery.as_ref(),
                    Some(&collected.agents),
                    Some(&collected.network),
                    Some(utc_ms()),
                )
                .ok_or(LinkError::Timeout("sending: the link is no longer paired"))?;
            self.port.write_all(&frame)?;
            observer.on_snapshot(&collected);
            next += SNAPSHOT_PERIOD;
            while Instant::now() < next {
                match observer.poll() {
                    ServeAction::Continue => {}
                    ServeAction::Stop => return Ok(ServeEnd::Stopped),
                    ServeAction::Command(operation) => {
                        let result = self.command(operation);
                        let done = matches!(
                            result,
                            Ok(CommandResult::Applied | CommandResult::AlreadyApplied)
                        );
                        observer.on_command(operation, result);
                        if done && operation == CommandOperation::Unpair {
                            return Ok(ServeEnd::Unpaired);
                        }
                        // The command may have taken seconds: restart the pace.
                        next = Instant::now();
                        last_wall = wall();
                    }
                }
                let wait = next.saturating_duration_since(Instant::now());
                let n = self
                    .port
                    .read_timeout(&mut buf, wait.min(Duration::from_millis(100)))?;
                for event in self.client.receive(&buf[..n]).events {
                    if let Event::Refused(reason) = event {
                        return Err(LinkError::Refused(reason));
                    }
                }
            }
        }
    }

    /// Sends a command and waits for its reply, retrying with the same
    /// identity so a lost frame cannot apply it twice.
    pub fn command(&mut self, operation: CommandOperation) -> Result<CommandResult, LinkError> {
        let (identity, frame) = self
            .client
            .command(operation)
            .ok_or(LinkError::Timeout("sending: the link is not paired"))?;
        self.port.write_all(&frame)?;
        let mut buf = [0u8; 512];
        for _attempt in 0..5 {
            let deadline = Instant::now() + Duration::from_secs(1);
            while Instant::now() < deadline {
                let n = self
                    .port
                    .read_timeout(&mut buf, Duration::from_millis(100))?;
                for event in self.client.receive(&buf[..n]).events {
                    if let Event::CommandReply {
                        command_identity,
                        result,
                        ..
                    } = event
                    {
                        if command_identity == identity {
                            return Ok(result);
                        }
                    }
                }
            }
            if let Some(retry) = self.client.retry_command(identity, operation) {
                self.port.write_all(&retry)?;
            }
        }
        Err(LinkError::Timeout("waiting for the command reply"))
    }
}

fn sleep_cancellable(total: Duration, cancel: &mut dyn FnMut() -> bool) -> Result<(), LinkError> {
    let end = Instant::now() + total;
    while Instant::now() < end {
        if cancel() {
            return Err(LinkError::Cancelled);
        }
        sleep(
            end.saturating_duration_since(Instant::now())
                .min(Duration::from_millis(50)),
        );
    }
    Ok(())
}

/// Why [`Link::serve`] returned without an error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServeEnd {
    Stopped,
    /// A sleep/wake gap: reconnect with a fresh session.
    Resync,
    /// The display accepted an unpair.
    Unpaired,
}

pub enum ServeAction {
    Continue,
    Stop,
    Command(CommandOperation),
}

/// The caller's view of a running session.
pub trait ServeObserver {
    /// Called after each snapshot was written.
    fn on_snapshot(&mut self, collected: &Collected);
    /// Polled about every 100 ms between snapshots.
    fn poll(&mut self) -> ServeAction {
        ServeAction::Continue
    }
    fn on_command(
        &mut self,
        _operation: CommandOperation,
        _result: Result<CommandResult, LinkError>,
    ) {
    }
}

/// All metric collectors, kept across reconnects so rates stay continuous.
pub struct Collectors {
    cpu: CpuCollector,
    ram: RamCollector,
    storage: StorageCollector,
    battery: BatteryCollector,
    agents: AgentsCollector,
    network: NetworkCollector,
}

/// One second of every wire group, plus what the CLI prints about it.
pub struct Collected {
    pub cpu: CpuSnapshot,
    pub ram: RamSnapshot,
    pub storage: Option<StorageSnapshot>,
    pub battery: Option<BatterySnapshot>,
    pub agents: AgentsSnapshot,
    pub network: NetworkSnapshot,
    cpu_per_mille: Option<u16>,
    ram_bytes: Option<(u64, u64)>,
}

impl Collectors {
    pub fn new(provider: Box<dyn WifiContextProvider>) -> Self {
        Self {
            cpu: CpuCollector::new(),
            ram: RamCollector::new(),
            storage: StorageCollector::new(),
            battery: BatteryCollector::new(),
            agents: AgentsCollector::new(),
            network: platform_macos::network::collector(provider),
        }
    }

    pub fn collect(&mut self, now_ms: u64) -> Collected {
        let sample = self.cpu.collect(now_ms);
        let cpu = CpuSnapshot {
            availability: availability(sample.availability),
            utilization_per_mille: sample.value.unwrap_or(0),
            source_age_ms: 0,
            freshness: Freshness::Fresh,
            configuration_revision: 0,
        };
        let ram_sample = self.ram.collect(now_ms);
        let ram = RamSnapshot {
            availability: availability(ram_sample.availability),
            used_bytes: ram_sample.value.map_or(0, |r| r.used_bytes),
            total_bytes: ram_sample.value.map_or(0, |r| r.total_bytes),
            source_age_ms: 0,
            freshness: Freshness::Fresh,
        };
        Collected {
            cpu,
            ram,
            storage: storage_snapshot(self.storage.collect(now_ms).value),
            battery: battery_snapshot(self.battery.collect(now_ms).value),
            agents: agents_snapshot(self.agents.collect(now_ms)),
            network: network_snapshot(self.network.collect(now_ms).value),
            cpu_per_mille: sample.value,
            ram_bytes: ram_sample.value.map(|r| (r.used_bytes, r.total_bytes)),
        }
    }
}

impl Collected {
    /// Per-metric-group availability, for the status summary.
    pub fn group_availability(&self) -> Vec<(&'static str, Availability)> {
        let mut groups = vec![
            ("cpu", self.cpu.availability),
            ("ram", self.ram.availability),
        ];
        if let Some(g) = &self.storage {
            groups.push(("storage", worst(g.capacity_availability, g.io_availability)));
        }
        if let Some(g) = &self.battery {
            groups.push((
                "battery",
                worst(g.charge_availability, g.condition_availability),
            ));
        }
        groups.push(("agents", self.agents.availability));
        groups.push(("network", self.network.traffic_availability));
        groups
    }

    /// The foreground CLI's per-second report. States and numbers only: never
    /// a path, process name, MAC address, SSID or BSSID.
    pub fn log_lines(&self, drops: Option<String>) -> Vec<String> {
        let mut lines = Vec::new();
        if let Some(g) = &self.storage {
            lines.push(format!(
                "storage {:.1} / {:.1} GB  read {} B/s  write {} B/s",
                (g.total_bytes - g.available_bytes) as f64 / 1e9,
                g.total_bytes as f64 / 1e9,
                g.read_bytes_per_s,
                g.write_bytes_per_s
            ));
        }
        if let Some(g) = &self.battery {
            lines.push(format!(
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
        let agents = &self.agents;
        lines.push(format!(
            "agents {:?}: codex {} claude {} grok {}",
            agents.availability, agents.state[0], agents.state[1], agents.state[2]
        ));
        let network = &self.network;
        lines.push(format!(
            "network {:?} kind {:?}{} rate {} context {:?} coverage {:?} ch {} station {} ssid {} bssid {}{}",
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
            drops.unwrap_or_default()
        ));
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

/// The less healthy of two independent halves of one group.
fn worst(a: Availability, b: Availability) -> Availability {
    if a == Availability::Available {
        b
    } else {
        a
    }
}

fn availability(a: MetricAvailability) -> Availability {
    match a {
        MetricAvailability::Available => Availability::Available,
        MetricAvailability::Unsupported => Availability::Unsupported,
        MetricAvailability::Absent => Availability::Absent,
        MetricAvailability::TemporarilyUnavailable => Availability::TemporarilyUnavailable,
        MetricAvailability::Error => Availability::Error,
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
            None => match availability(reading.capacity.availability) {
                Availability::Available => Availability::Error, // no value behind it
                other => other,
            },
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

/// The wire group for one second of battery collection. A half that is not
/// available carries zero values (times and cycle counts the unknown sentinel)
/// and its availability, so a batteryless host is `Absent`, never a 0% charge.
/// A condition source that lacks one field reports that field as 0 (capacity)
/// or the sentinel (cycles): a working battery has no 0% health capacity.
fn battery_snapshot(reading: Option<BatteryReading>) -> Option<BatterySnapshot> {
    let reading = reading?;
    let no_value = |a: MetricAvailability| match availability(a) {
        Availability::Available => Availability::Error, // no value behind it
        other => other,
    };
    let charge = reading.charge.value;
    let condition = reading.condition.value;
    Some(BatterySnapshot {
        charge_availability: if charge.is_some() {
            Availability::Available
        } else {
            no_value(reading.charge.availability)
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
            no_value(reading.condition.availability)
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
            availability: match availability(sample.availability) {
                Availability::Available => Availability::Error, // no value behind it
                other => other,
            },
            source_age_ms: 0,
            state: [0; 3],
        },
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

fn protocol_error(error: protocol::Error) -> LinkError {
    LinkError::Io(io::Error::new(
        io::ErrorKind::InvalidData,
        format!("encode failed: {error:?}"),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use metrics::battery::{
        BatteryChargeReading, BatteryCondition, BatteryConditionReading, BatteryState,
    };
    use metrics::MetricSample;

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
