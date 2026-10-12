// SPDX-License-Identifier: GPL-3.0-only

//! Host session over the board's serial port, shared by the foreground CLI
//! and the per-user daemon: handshake, pairing, one-second complete snapshots,
//! and the unpair / calibration-reset commands. The byte channel is a trait so
//! the daemon's integration tests can drive a scripted device.

use std::io;
use std::thread::sleep;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use metrics::wifi_context::WifiContextProvider;
use protocol::{Availability, CommandOperation, CommandResult, CpuSnapshot, Freshness, Snapshot};
use transport::client::{Event, HostClient, Phase, Refusal};
use transport::serial::SerialPort;

use crate::groups::{self, Group, Report};
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

/// Whether a clock moved from `last` to `now` by more than a tick can explain.
fn slept(last: u64, now: u64) -> bool {
    now.saturating_sub(last) > SNAPSHOT_PERIOD.as_millis() as u64 + WAKE_GAP_MS
}

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
        let (mut last_wall, mut last_mono) = (wall(), clock());
        let mut buf = [0u8; 512];
        loop {
            let (wall_now, mono_now) = (wall(), clock());
            // The pacing `Instant` stops while the Mac sleeps; the wall clock
            // and the sleep-inclusive monotonic clock do not.
            if slept(last_wall, wall_now) || slept(last_mono, mono_now) {
                // The Mac slept: whatever the display saw is stale and the
                // USB link may have been reset. No rate may span the sleep and
                // no cached sample may be re-sent as current: start over with
                // a new session and freshly read sources.
                collectors.on_wake();
                return Ok(ServeEnd::Resync);
            }
            (last_wall, last_mono) = (wall_now, mono_now);
            let collected = collectors.collect(mono_now);
            self.client
                .set_local_offset_minutes(platform_macos::time::local_offset_minutes());
            let frame = self
                .client
                .snapshot(&collected.snapshot, Some(utc_ms()))
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
                        (last_wall, last_mono) = (wall(), clock());
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

/// All metric groups (see [`crate::groups`]), kept across reconnects so rates
/// stay continuous (except across a sleep: see [`Collectors::on_wake`]).
pub struct Collectors {
    groups: Vec<Box<dyn Group>>,
}

/// One second of every wire group, plus what the status summary and the CLI
/// report about it.
pub struct Collected {
    pub snapshot: Snapshot,
    report: Report,
}

impl Collectors {
    pub fn new(provider: Box<dyn WifiContextProvider>) -> Self {
        Self::from_groups(groups::all(provider))
    }

    /// Collectors over given groups (tests use fakes).
    pub(crate) fn from_groups(groups: Vec<Box<dyn Group>>) -> Self {
        Self { groups }
    }

    /// The Mac slept: every rate re-baselines and every cached slow source
    /// is read again before the next snapshot (`Collector::on_wake`).
    pub fn on_wake(&mut self) {
        for group in &mut self.groups {
            group.wake();
        }
    }

    pub fn collect(&mut self, now_ms: u64) -> Collected {
        // The CPU group overwrites this placeholder.
        let mut snapshot = Snapshot::new(CpuSnapshot {
            availability: Availability::TemporarilyUnavailable,
            utilization_per_mille: 0,
            source_age_ms: 0,
            freshness: Freshness::Fresh,
            configuration_revision: 0,
        });
        let mut report = Report::default();
        for group in &mut self.groups {
            group.collect_into(now_ms, &mut snapshot, &mut report);
        }
        Collected { snapshot, report }
    }
}

impl Collected {
    /// Per-metric-group availability, for the status summary.
    pub fn group_availability(&self) -> Vec<(&'static str, Availability)> {
        self.report.group_availability()
    }

    /// The foreground CLI's per-second report. States and numbers only:
    /// never a path, process name, MAC address, SSID or BSSID.
    pub fn log_lines(&self, drops: Option<String>) -> Vec<String> {
        self.report.log_lines(drops)
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
    use metrics::battery::{BatteryChargeReading, BatteryState};
    use metrics::network::NetworkCollector;
    use metrics::wifi_context::ContextAvailability;
    use platform_macos::agents::AgentsCollector;
    use platform_macos::battery::BatteryCollector;
    use platform_macos::cpu::CpuCollector;
    use platform_macos::ram::RamCollector;
    use platform_macos::storage::StorageCollector;
    use protocol::AssociationContext;

    #[test]
    fn a_gap_on_either_clock_means_the_mac_slept() {
        assert!(!slept(10_000, 11_000));
        assert!(!slept(10_000, 10_000 + 6_000), "a late tick is not a sleep");
        assert!(slept(10_000, 10_000 + 6_001));
        assert!(!slept(10_000, 9_000), "a clock set back is not a sleep");
    }

    /// Every slow source and rate of the real collectors, over fakes: a
    /// simulated two-hour sleep (a monotonic jump plus the wake `serve`
    /// reports) never lets a pre-sleep reading through as current.
    #[test]
    fn a_simulated_sleep_restarts_every_rate_and_refreshes_or_ages_every_cached_source() {
        use metrics::network::{
            InterfaceCounters, InterfaceInfo, LinkKind, NetworkSystem, SystemView,
        };
        use metrics::storage::{DiskCounters, StorageCapacityReading};
        use metrics::wifi_context::{
            AssociationContext as Context, ContextTracker, TrackerProvider, WifiBand,
        };
        use metrics::CpuTicks;
        use platform_macos::battery::ChargeProbe;
        use std::cell::Cell;
        use std::rc::Rc;
        use std::sync::atomic::{AtomicU32, Ordering};
        use std::sync::{Arc, Mutex};

        static CPU_READS: AtomicU32 = AtomicU32::new(0);
        fn ticks() -> Option<CpuTicks> {
            let n = CPU_READS.fetch_add(1, Ordering::SeqCst);
            Some(CpuTicks {
                user: 10 * n,
                system: 0,
                idle: 90 * n,
                nice: 0,
            })
        }
        struct Wifi(u64);
        impl NetworkSystem for Wifi {
            fn read(&mut self) -> Option<SystemView> {
                self.0 += 1_000;
                Some(SystemView {
                    interfaces: vec![InterfaceInfo {
                        name: "en0".into(),
                        index: 6,
                        kind: LinkKind::Wifi,
                        up_running: true,
                        link_active: true,
                        station: Some([2, 0, 0, 0, 0, 6]),
                        counters: Some(InterfaceCounters {
                            rx_bytes: self.0,
                            tx_bytes: self.0,
                        }),
                    }],
                    default_routes: vec![metrics::network::DefaultRoute {
                        interface: "en0".into(),
                        scoped: false,
                    }],
                })
            }
        }
        // After the sleep the charge and process listing fail; capacity works.
        let awake = Rc::new(Cell::new(true));
        let (charge_ok, scan_ok) = (awake.clone(), awake.clone());
        let capacity_reads = Rc::new(Cell::new(0u32));
        let capacity_seen = capacity_reads.clone();
        let mut disk = 0u64;
        let tracker = Arc::new(Mutex::new(ContextTracker::default()));
        let mut collectors = Collectors::from_groups(vec![
            Box::new(CpuCollector::with_source(ticks)),
            Box::new(RamCollector::new()),
            Box::new(StorageCollector::with_sources(
                Box::new(move || {
                    capacity_seen.set(capacity_seen.get() + 1);
                    Some(StorageCapacityReading {
                        total_bytes: 500,
                        available_bytes: 200,
                    })
                }),
                Box::new(move || {
                    disk += 4_096;
                    Some(vec![(
                        "disk0".into(),
                        DiskCounters {
                            read_bytes: disk,
                            write_bytes: disk,
                        },
                    )])
                }),
            )),
            Box::new(BatteryCollector::with_sources(
                Box::new(move || {
                    if charge_ok.get() {
                        ChargeProbe::Battery(BatteryChargeReading {
                            per_mille: 800,
                            state: BatteryState::Discharging,
                            time_remaining_min: Some(120),
                        })
                    } else {
                        ChargeProbe::Failed
                    }
                }),
                Box::new(|| Some("Battery Power".into())),
                Arc::new(|| Ok(metrics::battery::PowerProfile::NoBattery)),
            )),
            Box::new(AgentsCollector::with_source(
                Box::new(move || scan_ok.get().then(Vec::new)),
                501,
                "/Users/tester".into(),
            )),
            Box::new(NetworkCollector::new(
                Box::new(Wifi(0)),
                Box::new(TrackerProvider(tracker.clone())),
            )),
        ]);
        tracker.lock().unwrap().on_context(
            Context {
                availability: ContextAvailability::Available,
                band: Some(WifiBand::Ghz2_4),
                channel: Some(6),
                ssid: Some(b"CYD".to_vec()),
                bssid: None,
            },
            0,
            1_000,
        );
        collectors.collect(1_000);
        let before = collectors.collect(2_000).snapshot;
        assert_eq!(before.cpu.availability, Availability::Available);
        let storage = before.storage.clone().unwrap();
        assert_eq!(storage.io_availability, Availability::Available);
        assert_eq!(
            before.network.as_ref().unwrap().traffic_availability,
            Availability::Available
        );
        assert_eq!(before.battery.clone().unwrap().charge_age_ms, 1_000);
        assert_eq!(
            before.agents.as_ref().unwrap().availability,
            Availability::Available
        );
        assert_eq!(
            (
                before.network.as_ref().unwrap().context_availability,
                before.network.as_ref().unwrap().context_age_ms
            ),
            (AssociationContext::Available, 1_000)
        );

        // Two hours asleep: the daemon's clock jumps by the sleep.
        awake.set(false);
        collectors.on_wake();
        let after = collectors.collect(2_000 + 7_200_000).snapshot;
        // No rate is averaged across the sleep.
        assert_eq!(after.cpu.availability, Availability::TemporarilyUnavailable);
        let storage = after.storage.clone().unwrap();
        assert_eq!(
            storage.io_availability,
            Availability::TemporarilyUnavailable
        );
        assert_eq!(
            after.network.as_ref().unwrap().traffic_availability,
            Availability::TemporarilyUnavailable
        );
        // Capacity was read again; the charge re-read failed, so the 80% from
        // before the sleep carries its full age and the display shows it stale.
        assert_eq!((capacity_reads.get(), storage.capacity_age_ms), (2, 0));
        let battery = after.battery.clone().unwrap();
        assert_eq!(
            (battery.charge_per_mille, battery.charge_age_ms),
            (800, 7_201_000)
        );
        // The failed rescan leaves presence past its retention: an error.
        assert_eq!(
            after.agents.as_ref().unwrap().availability,
            Availability::Error
        );
        // The context from before the sleep is dropped, not re-sent as fresh.
        assert_eq!(
            after.network.as_ref().unwrap().context_availability,
            AssociationContext::Error
        );
        assert!(
            after.network.as_ref().unwrap().ssid.is_empty()
                && after.network.as_ref().unwrap().channel == 0
        );
    }
}
