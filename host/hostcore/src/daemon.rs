// SPDX-License-Identifier: GPL-3.0-only

//! The per-user daemon: owns the single serial port, runs the paired-session
//! snapshot loop, hosts the WiFi context provider socket and serves the
//! local status/control IPC. It never exits because a provider, a permission
//! or the setup app is absent, and it survives unplug/replug and sleep/wake by
//! reconnecting with a fresh session and an immediate complete snapshot.

use std::io;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use metrics::wifi_context::{
    ContextTracker, FixtureProvider, PermissionStatus, WifiContextProvider,
};
use platform_macos::ipc::current_uid;
use platform_macos::wifi_context_server::{
    ServerConfig, Stats as ProviderStats, WifiContextServer,
};
use protocol::{Availability, CommandOperation, CommandResult};
use transport::client::Refusal;
use transport::serial::SerialPort;

use crate::control::{
    error_response, json_string, ok_response, ControlServer, ControlStats, Handler, JsonObject,
    Request,
};
use crate::identity::{self, short_form};
use crate::link::{
    Channel, Collected, Collectors, ConnectOptions, Link, LinkError, Progress, ServeAction,
    ServeEnd, ServeObserver,
};
use crate::lock::{InstanceLock, LockError};
use crate::logging::Logger;

/// How long the control server waits for the supervisor to answer a command.
const COMMAND_TIMEOUT: Duration = Duration::from_secs(20);

pub use platform_macos::time::NowMs;

/// Monotonic milliseconds since the daemon started, counting time the Mac
/// spent asleep ([`platform_macos::time::monotonic_ms`]), with an injectable
/// skew so tests can jump time (provider staleness, sleep/wake) without
/// waiting. Every sample age, staleness limit and rate interval uses it.
#[derive(Clone)]
pub struct Clock {
    origin_ms: u64,
    skew_ms: Arc<AtomicU64>,
}

impl Default for Clock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock {
    pub fn new() -> Self {
        Self {
            origin_ms: platform_macos::time::monotonic_ms(),
            skew_ms: Arc::new(AtomicU64::new(0)),
        }
    }

    pub fn now_ms(&self) -> u64 {
        platform_macos::time::monotonic_ms().saturating_sub(self.origin_ms)
            + self.skew_ms.load(Ordering::Relaxed)
    }

    /// This clock as a shareable function, for the provider socket's tracker.
    pub fn now_fn(&self) -> NowMs {
        let clock = self.clone();
        Arc::new(move || clock.now_ms())
    }

    /// Wall-clock milliseconds; a jump larger than the snapshot period means
    /// the Mac slept.
    pub fn wall_ms(&self) -> u64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_millis() as u64)
            + self.skew_ms.load(Ordering::Relaxed)
    }

    /// Test hook: jumps both clocks forward, as a sleep does.
    pub fn advance(&self, ms: u64) {
        self.skew_ms.fetch_add(ms, Ordering::Relaxed);
    }
}

pub trait PortOpener: Send + Sync {
    fn open(&self, path: &str) -> io::Result<Box<dyn Channel>>;
}

pub struct SerialOpener;

impl PortOpener for SerialOpener {
    fn open(&self, path: &str) -> io::Result<Box<dyn Channel>> {
        Ok(Box::new(SerialPort::open(path)?))
    }
}

pub trait PortDiscovery: Send + Sync {
    /// Serial devices that could be a Monitor Device, sorted.
    fn candidates(&self) -> Vec<String>;
    fn exists(&self, path: &str) -> bool;
}

/// Looks for the USB serial nodes an ESP32 board's bridge chip creates.
pub struct SystemDiscovery;

impl PortDiscovery for SystemDiscovery {
    fn candidates(&self) -> Vec<String> {
        let mut found: Vec<String> = std::fs::read_dir("/dev")
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|entry| entry.file_name().into_string().ok())
            .filter(|name| {
                [
                    "cu.usbserial",
                    "cu.usbmodem",
                    "cu.wchusbserial",
                    "cu.SLAB_USBtoUART",
                ]
                .iter()
                .any(|prefix| name.starts_with(prefix))
            })
            .map(|name| format!("/dev/{name}"))
            .collect();
        found.sort();
        found
    }

    fn exists(&self, path: &str) -> bool {
        std::path::Path::new(path).exists()
    }
}

pub struct DaemonConfig {
    pub run_dir: PathBuf,
    /// Where the Pairing Identity is kept.
    pub state_dir: PathBuf,
    /// A fixed serial device; otherwise the single discovered candidate.
    pub port: Option<String>,
    pub opener: Arc<dyn PortOpener>,
    pub discovery: Arc<dyn PortDiscovery>,
    pub clock: Clock,
    pub boot_wait: Duration,
    /// Pause before reconnecting after a lost link.
    pub retry_wait: Duration,
    pub handshake_deadline: Duration,
    pub logger: Arc<Logger>,
}

impl DaemonConfig {
    /// The real system: serial ports under `/dev`, the default directories.
    pub fn system(logger: Arc<Logger>, port: Option<String>) -> Self {
        Self {
            run_dir: crate::paths::run_dir(),
            state_dir: identity::state_dir(),
            port,
            opener: Arc::new(SerialOpener),
            discovery: Arc::new(SystemDiscovery),
            clock: Clock::new(),
            boot_wait: crate::link::BOOT_WAIT,
            retry_wait: Duration::from_secs(2),
            handshake_deadline: Duration::from_secs(75),
            logger,
        }
    }
}

#[derive(Debug)]
pub enum StartError {
    /// Another daemon or foreground run owns the serial port.
    AlreadyRunning {
        pid: Option<u32>,
    },
    Io(io::Error),
}

impl std::fmt::Display for StartError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            StartError::AlreadyRunning { pid } => LockError::Held { pid: *pid }.fmt(f),
            StartError::Io(e) => write!(f, "cannot start: {e}"),
        }
    }
}

/// The link state reported by `status`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinkState {
    NoDevice,
    DeviceFound,
    PairingRequired,
    PairedConnected,
    Stale,
}

impl LinkState {
    pub fn name(self) -> &'static str {
        match self {
            LinkState::NoDevice => "no_device",
            LinkState::DeviceFound => "device_found",
            LinkState::PairingRequired => "pairing_required",
            LinkState::PairedConnected => "paired_connected",
            LinkState::Stale => "stale",
        }
    }
}

struct LinkStatus {
    state: LinkState,
    detail: Option<String>,
    port: Option<String>,
    pairing_code: Option<u32>,
    pairing_identity: Option<String>,
    sessions: u64,
}

struct Shared {
    link: LinkStatus,
    last_snapshot_utc_ms: Option<u64>,
    /// [`Clock::now_ms`] of the latest snapshot.
    last_snapshot_ms: Option<u64>,
    snapshots: u64,
    groups: Vec<(&'static str, &'static str)>,
}

fn availability_name(a: Availability) -> &'static str {
    match a {
        Availability::Available => "available",
        Availability::Unsupported => "unsupported",
        Availability::Absent => "absent",
        Availability::TemporarilyUnavailable => "temporarily_unavailable",
        Availability::Error => "error",
    }
}

struct Command {
    request: Request,
    reply: SyncSender<String>,
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|p| p.into_inner())
}

/// Answers `status` and `monitors` directly from shared state, so they work
/// even during a 75 s pairing handshake; everything else goes to the
/// supervisor.
struct DaemonHandler {
    shared: Arc<Mutex<Shared>>,
    commands: Mutex<Sender<Command>>,
    stop: Arc<AtomicBool>,
    provider: Option<Arc<WifiContextServer>>,
    tracker: Option<Arc<Mutex<ContextTracker>>>,
    discovery: Arc<dyn PortDiscovery>,
    clock: Clock,
    started_ms: u64,
    version: &'static str,
}

fn opt_str(v: Option<&str>) -> String {
    v.map_or("null".into(), json_string)
}

impl DaemonHandler {
    fn status(&self, control: &ControlStats) -> String {
        let s = lock(&self.shared);
        let link = &s.link;
        let now = self.clock.now_ms();
        let metrics = s
            .groups
            .iter()
            .fold(JsonObject::new(), |o, (name, a)| o.str(name, a));
        let (provider_state, context, permission, provider_counters) =
            match (&self.provider, &self.tracker) {
                (Some(server), Some(tracker)) => {
                    let t = lock(tracker);
                    let reading = t.reading(now);
                    let stats: ProviderStats = server.stats();
                    let state = if t.is_connected() {
                        "connected"
                    } else if stats.accepted > 0 {
                        "disconnected"
                    } else {
                        "never_connected"
                    };
                    let counters = JsonObject::new()
                        .num("accepted", stats.accepted)
                        .num("replaced", stats.replaced)
                        .num("wrong_uid", stats.wrong_uid)
                        .num("malformed", stats.malformed)
                        .num("oversized", stats.oversized)
                        .num("out_of_order", stats.out_of_order)
                        .num("rate_limited", stats.rate_limited)
                        .num("bad_version", stats.bad_version)
                        .num("closed_for_bad_lines", stats.closed_for_bad_lines)
                        .num("applied", stats.applied)
                        .finish();
                    (
                        state,
                        reading.context.availability.name(),
                        t.permission().map(PermissionStatus::name),
                        counters,
                    )
                }
                _ => ("server_unavailable", "not_requested", None, "null".into()),
            };
        JsonObject::new()
            .object(
                "daemon",
                JsonObject::new()
                    .str("version", self.version)
                    .num("pid", std::process::id().into())
                    .num("uptime_s", now.saturating_sub(self.started_ms) / 1000)
                    .num("uid", current_uid().into()),
            )
            .object(
                "link",
                JsonObject::new()
                    .str("state", link.state.name())
                    .opt_str("detail", link.detail.as_deref())
                    .opt_str("port", link.port.as_deref())
                    .opt_str("pairing_identity", link.pairing_identity.as_deref())
                    .opt_str(
                        "pairing_code",
                        link.pairing_code.map(|c| format!("{c:06}")).as_deref(),
                    )
                    .num("sessions", link.sessions)
                    .num("snapshots", s.snapshots)
                    .opt_num("last_snapshot_utc_ms", s.last_snapshot_utc_ms)
                    .opt_num(
                        "last_snapshot_age_ms",
                        s.last_snapshot_ms.map(|t| now.saturating_sub(t)),
                    ),
            )
            .object("metrics", metrics)
            .object(
                "wifi_context",
                JsonObject::new()
                    .str("provider", provider_state)
                    .str("availability", context)
                    .opt_str("permission", permission),
            )
            .object(
                "counters",
                JsonObject::new()
                    .raw("provider", &provider_counters)
                    .object(
                        "control",
                        JsonObject::new()
                            .num("accepted", control.accepted)
                            .num("requests", control.requests)
                            .num("wrong_uid", control.wrong_uid)
                            .num("malformed", control.malformed)
                            .num("oversized", control.oversized)
                            .num("bad_version", control.bad_version)
                            .num("unknown_command", control.unknown_command)
                            .num("busy", control.busy)
                            .num("closed_for_bad_lines", control.closed_for_bad_lines),
                    ),
            )
            .finish()
    }

    fn monitors(&self) -> String {
        let s = lock(&self.shared);
        let list: Vec<String> = self
            .discovery
            .candidates()
            .iter()
            .map(|port| {
                let current = s.link.port.as_deref() == Some(port.as_str());
                format!(
                    "{{\"port\":{},\"pairing_identity\":{},\"state\":{}}}",
                    json_string(port),
                    opt_str(if current {
                        s.link.pairing_identity.as_deref()
                    } else {
                        None
                    }),
                    json_string(if current {
                        s.link.state.name()
                    } else {
                        "unknown"
                    })
                )
            })
            .collect();
        format!("[{}]", list.join(","))
    }
}

impl Handler for DaemonHandler {
    fn handle(&self, request: Request, control: &ControlStats) -> String {
        match request {
            Request::Status => ok_response("status", Some(("status", &self.status(control)))),
            Request::Monitors => ok_response("monitors", Some(("monitors", &self.monitors()))),
            Request::Stop => {
                self.stop.store(true, Ordering::Relaxed);
                ok_response("stop", None)
            }
            other => {
                let name = match &other {
                    Request::PairStart { .. } => "pair_start",
                    Request::PairCancel => "pair_cancel",
                    Request::Unpair => "unpair",
                    Request::ResetCalibration => "reset_calibration",
                    _ => "forget",
                };
                let (reply, wait) = mpsc::sync_channel(1);
                if lock(&self.commands)
                    .send(Command {
                        request: other,
                        reply,
                    })
                    .is_err()
                {
                    return error_response("stopping", "the daemon is stopping");
                }
                match wait.recv_timeout(COMMAND_TIMEOUT) {
                    Ok(response) => response,
                    Err(_) => error_response("timeout", &format!("{name} timed out")),
                }
            }
        }
    }
}

/// What the supervisor does with the port, decided each loop.
enum PortChoice {
    None,
    Ambiguous(usize),
    One(String),
}

#[derive(Default)]
struct PairControl {
    requested: bool,
    port: Option<String>,
    /// A port whose last attempt ended in a refusal: not retried (each open
    /// resets the board) until it is replugged or the Operator acts.
    parked: Option<String>,
}

struct Supervisor {
    config: DaemonConfig,
    shared: Arc<Mutex<Shared>>,
    commands: Receiver<Command>,
    stop: Arc<AtomicBool>,
    pair: Mutex<PairControl>,
    provider: Option<Box<dyn WifiContextProvider + Send>>,
}

impl Supervisor {
    fn stopped(&self) -> bool {
        self.stop.load(Ordering::Relaxed)
    }

    fn set_link(
        &self,
        state: LinkState,
        detail: Option<&str>,
        port: Option<&str>,
        code: Option<u32>,
    ) {
        let mut s = lock(&self.shared);
        let changed = s.link.state != state
            || s.link.detail.as_deref() != detail
            || s.link.port.as_deref() != port;
        s.link.state = state;
        s.link.detail = detail.map(String::from);
        s.link.port = port.map(String::from);
        s.link.pairing_code = code;
        if state != LinkState::PairedConnected {
            s.groups.clear();
        }
        drop(s);
        if changed {
            self.config.logger.line(&format!(
                "link {}{}{}",
                state.name(),
                port.map_or(String::new(), |p| format!(" on {p}")),
                detail.map_or(String::new(), |d| format!(": {d}"))
            ));
        }
    }

    fn set_identity(&self, device: Option<[u8; 16]>) {
        lock(&self.shared).link.pairing_identity = device.as_ref().map(short_form);
    }

    fn reply(command: Command, response: String) {
        let _ = command.reply.send(response);
    }

    /// Handles a command that arrives with no live link.
    fn handle_idle(&self, command: Command) {
        let response = match &command.request {
            Request::PairStart { port } => {
                match identity::load_or_create_in(&self.config.state_dir) {
                    Ok(id) if id.device.is_some() => error_response(
                        "already_paired",
                        "a display is already paired; unpair it first",
                    ),
                    Ok(_) => {
                        let mut pair = lock(&self.pair);
                        pair.requested = true;
                        pair.port = port.clone();
                        pair.parked = None;
                        ok_response("pair_start", None)
                    }
                    Err(e) => error_response("identity", &e.to_string()),
                }
            }
            Request::PairCancel => {
                let mut pair = lock(&self.pair);
                pair.requested = false;
                pair.port = None;
                ok_response("pair_cancel", None)
            }
            Request::Forget => match self.forget() {
                Ok(()) => ok_response("forget", None),
                Err(e) => error_response("identity", &e.to_string()),
            },
            _ => error_response("not_connected", "no paired display is connected"),
        };
        Self::reply(command, response);
    }

    fn forget(&self) -> io::Result<()> {
        let mut id = identity::load_or_create_in(&self.config.state_dir)?;
        id.device = None;
        identity::save_in(&self.config.state_dir, &id)?;
        self.set_identity(None);
        lock(&self.pair).parked = None;
        self.config.logger.line("forgot the stored display");
        Ok(())
    }

    /// Waits up to `duration`, returning early after handling one command.
    fn idle_wait(&self, duration: Duration) {
        let end = Instant::now() + duration;
        while !self.stopped() && Instant::now() < end {
            let slice = end
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(50));
            match self.commands.recv_timeout(slice) {
                Ok(command) => {
                    self.handle_idle(command);
                    return;
                }
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => thread::sleep(slice),
            }
        }
    }

    fn pick_port(&self) -> PortChoice {
        let pair_port = lock(&self.pair).port.clone();
        if let Some(port) = pair_port.or_else(|| self.config.port.clone()) {
            return if self.config.discovery.exists(&port) {
                PortChoice::One(port)
            } else {
                PortChoice::None
            };
        }
        let mut found = self.config.discovery.candidates();
        match found.len() {
            0 => PortChoice::None,
            1 => PortChoice::One(found.remove(0)),
            n => PortChoice::Ambiguous(n),
        }
    }

    fn run(mut self) {
        let provider = self
            .provider
            .take()
            .unwrap_or_else(|| Box::new(FixtureProvider::absent()));
        let mut collectors = Collectors::new(provider);
        while !self.stopped() {
            let choice = self.pick_port();
            let port = match choice {
                PortChoice::None => {
                    lock(&self.pair).parked = None;
                    self.set_link(LinkState::NoDevice, None, None, None);
                    self.idle_wait(Duration::from_millis(500));
                    continue;
                }
                PortChoice::Ambiguous(n) => {
                    self.set_link(
                        LinkState::DeviceFound,
                        Some(&format!(
                            "{n} candidate devices; name one with --port or pair_start"
                        )),
                        None,
                        None,
                    );
                    self.idle_wait(Duration::from_millis(500));
                    continue;
                }
                PortChoice::One(port) => port,
            };
            let mut id = match identity::load_or_create_in(&self.config.state_dir) {
                Ok(id) => id,
                Err(e) => {
                    self.set_link(
                        LinkState::Stale,
                        Some(&format!("cannot read the pairing identity: {e}")),
                        Some(&port),
                        None,
                    );
                    self.idle_wait(self.config.retry_wait);
                    continue;
                }
            };
            self.set_identity(id.device);
            if lock(&self.pair).parked.as_deref() == Some(port.as_str()) {
                self.idle_wait(Duration::from_millis(500));
                continue;
            }
            let pairing = lock(&self.pair).requested;
            if id.device.is_none() && !pairing {
                self.set_link(
                    LinkState::PairingRequired,
                    Some("a display is attached; start pairing to use it"),
                    Some(&port),
                    None,
                );
                self.idle_wait(Duration::from_millis(500));
                continue;
            }
            self.set_link(
                LinkState::DeviceFound,
                Some("connecting"),
                Some(&port),
                None,
            );
            let channel = match self.config.opener.open(&port) {
                Ok(channel) => channel,
                Err(e) => {
                    self.set_link(
                        LinkState::Stale,
                        Some(&format!("cannot open the port: {e}")),
                        Some(&port),
                        None,
                    );
                    self.idle_wait(self.config.retry_wait);
                    continue;
                }
            };
            lock(&self.shared).link.sessions += 1;
            let connected = {
                let mut cancel = || {
                    if self.stopped() {
                        return true;
                    }
                    while let Ok(command) = self.commands.try_recv() {
                        if command.request == Request::PairCancel {
                            let mut pair = lock(&self.pair);
                            pair.requested = false;
                            pair.port = None;
                            Self::reply(command, ok_response("pair_cancel", None));
                            return true;
                        }
                        Self::reply(
                            command,
                            error_response("busy", "a connection attempt is in progress"),
                        );
                    }
                    false
                };
                let mut progress = |p: Progress| match p {
                    Progress::Opened | Progress::Negotiated { .. } => self.set_link(
                        LinkState::DeviceFound,
                        Some("connecting"),
                        Some(&port),
                        None,
                    ),
                    Progress::AwaitingConfirmation { code } => self.set_link(
                        LinkState::PairingRequired,
                        Some("confirm the matching code on the display"),
                        Some(&port),
                        Some(code),
                    ),
                    Progress::Paired => {}
                };
                Link::connect_with(
                    channel,
                    &mut id,
                    ConnectOptions {
                        boot_wait: self.config.boot_wait,
                        deadline: self.config.handshake_deadline,
                        cancel: &mut cancel,
                        on_progress: &mut progress,
                        save_identity: &mut |id| identity::save_in(&self.config.state_dir, id),
                    },
                )
            };
            let mut link = match connected {
                Ok(link) => link,
                Err(error) => {
                    self.after_failed_connect(error, &port);
                    continue;
                }
            };
            {
                let mut pair = lock(&self.pair);
                pair.requested = false;
                pair.port = None;
            }
            self.set_identity(id.device);
            self.set_link(LinkState::PairedConnected, None, Some(&port), None);
            let clock = self.config.clock.clone();
            let wall = self.config.clock.clone();
            let mut observer = Observer {
                sup: &self,
                pending: None,
            };
            let outcome = link.serve(
                &mut collectors,
                &move || clock.now_ms(),
                &move || wall.wall_ms(),
                &mut observer,
            );
            match outcome {
                Ok(ServeEnd::Stopped) => break,
                Ok(ServeEnd::Resync) => {
                    self.set_link(
                        LinkState::Stale,
                        Some("resynchronizing after sleep/wake"),
                        Some(&port),
                        None,
                    );
                }
                Ok(ServeEnd::Unpaired) => {
                    if let Err(e) = self.forget() {
                        self.config
                            .logger
                            .line(&format!("cannot forget the display: {e}"));
                    }
                    self.set_link(
                        LinkState::PairingRequired,
                        Some("unpaired; start pairing to use a display"),
                        Some(&port),
                        None,
                    );
                    lock(&self.pair).parked = Some(port);
                }
                Err(error) => {
                    self.set_link(
                        LinkState::Stale,
                        Some(&error.to_string()),
                        Some(&port),
                        None,
                    );
                    if let LinkError::Refused(_) = error {
                        lock(&self.pair).parked = Some(port);
                    } else {
                        self.idle_wait(self.config.retry_wait);
                    }
                }
            }
        }
        self.set_link(LinkState::NoDevice, Some("daemon stopped"), None, None);
    }

    fn after_failed_connect(&self, error: LinkError, port: &str) {
        match error {
            LinkError::Cancelled => {}
            LinkError::Io(e) => {
                self.set_link(
                    LinkState::Stale,
                    Some(&format!("serial error: {e}")),
                    Some(port),
                    None,
                );
                self.idle_wait(self.config.retry_wait);
            }
            LinkError::Refused(Refusal::Incompatible) => {
                self.set_link(
                    LinkState::DeviceFound,
                    Some("the display speaks an incompatible protocol version"),
                    Some(port),
                    None,
                );
                lock(&self.pair).parked = Some(port.into());
            }
            other => {
                let pairing = matches!(
                    other,
                    LinkError::Refused(Refusal::NotPairedWithYou | Refusal::Declined)
                        | LinkError::Timeout("waiting for the display to pair")
                );
                self.set_link(
                    if pairing {
                        LinkState::PairingRequired
                    } else {
                        LinkState::Stale
                    },
                    Some(&other.to_string()),
                    Some(port),
                    None,
                );
                let mut pair = lock(&self.pair);
                pair.requested = false;
                pair.port = None;
                pair.parked = Some(port.into());
            }
        }
    }
}

struct Observer<'a> {
    sup: &'a Supervisor,
    pending: Option<(Command, CommandOperation)>,
}

impl ServeObserver for Observer<'_> {
    fn on_snapshot(&mut self, collected: &Collected) {
        let mut s = lock(&self.sup.shared);
        s.snapshots += 1;
        s.last_snapshot_ms = Some(self.sup.config.clock.now_ms());
        s.last_snapshot_utc_ms = Some(crate::link::utc_ms());
        s.groups = collected
            .group_availability()
            .into_iter()
            .map(|(name, a)| (name, availability_name(a)))
            .collect();
    }

    fn poll(&mut self) -> ServeAction {
        if self.sup.stopped() {
            return ServeAction::Stop;
        }
        let Ok(command) = self.sup.commands.try_recv() else {
            return ServeAction::Continue;
        };
        let operation = match command.request {
            Request::Unpair => CommandOperation::Unpair,
            Request::ResetCalibration => CommandOperation::CalibrationReset,
            Request::PairCancel => {
                Supervisor::reply(command, ok_response("pair_cancel", None));
                return ServeAction::Continue;
            }
            Request::Forget => {
                Supervisor::reply(
                    command,
                    error_response("paired_connected", "unpair the connected display instead"),
                );
                return ServeAction::Continue;
            }
            _ => {
                Supervisor::reply(
                    command,
                    error_response("already_paired", "a display is already paired"),
                );
                return ServeAction::Continue;
            }
        };
        self.pending = Some((command, operation));
        ServeAction::Command(operation)
    }

    fn on_command(&mut self, _op: CommandOperation, result: Result<CommandResult, LinkError>) {
        let Some((command, operation)) = self.pending.take() else {
            return;
        };
        let name = if operation == CommandOperation::Unpair {
            "unpair"
        } else {
            "reset_calibration"
        };
        let response = match result {
            Ok(CommandResult::Applied | CommandResult::AlreadyApplied) => ok_response(name, None),
            Ok(CommandResult::Rejected) => {
                error_response("rejected", "the display rejected the command")
            }
            Err(e) => error_response("link_error", &e.to_string()),
        };
        Supervisor::reply(command, response);
    }
}

/// A running daemon.
pub struct Daemon {
    _lock: InstanceLock,
    control: Option<ControlServer>,
    provider: Option<Arc<WifiContextServer>>,
    stop: Arc<AtomicBool>,
    supervisor: Option<JoinHandle<()>>,
    logger: Arc<Logger>,
}

impl Daemon {
    /// Takes the single-instance lock, starts the provider and control
    /// sockets and the supervisor thread. A provider or control socket that
    /// cannot start is logged and skipped: monitoring still runs.
    pub fn start(config: DaemonConfig) -> Result<Daemon, StartError> {
        let lock = match InstanceLock::acquire(&config.run_dir) {
            Ok(lock) => lock,
            Err(LockError::Held { pid }) => return Err(StartError::AlreadyRunning { pid }),
            Err(LockError::Io(e)) => return Err(StartError::Io(e)),
        };
        let logger = config.logger.clone();
        let clock = config.clock.clone();
        let provider = match WifiContextServer::start(ServerConfig {
            dir: config.run_dir.clone(),
            expected_uid: current_uid(),
            now_ms: clock.now_fn(),
        }) {
            Ok(server) => Some(Arc::new(server)),
            Err(e) => {
                logger.line(&format!(
                    "WiFi context provider socket unavailable ({e}); continuing without it"
                ));
                None
            }
        };
        let tracker = provider.as_ref().map(|p| p.provider().0);
        let stop = Arc::new(AtomicBool::new(false));
        let shared = Arc::new(Mutex::new(Shared {
            link: LinkStatus {
                state: LinkState::NoDevice,
                detail: None,
                port: None,
                pairing_code: None,
                pairing_identity: None,
                sessions: 0,
            },
            last_snapshot_utc_ms: None,
            last_snapshot_ms: None,
            snapshots: 0,
            groups: Vec::new(),
        }));
        let (tx, rx) = mpsc::channel();
        let handler = Arc::new(DaemonHandler {
            shared: shared.clone(),
            commands: Mutex::new(tx),
            stop: stop.clone(),
            provider: provider.clone(),
            tracker,
            discovery: config.discovery.clone(),
            clock: clock.clone(),
            started_ms: clock.now_ms(),
            version: env!("CARGO_PKG_VERSION"),
        });
        let control = match ControlServer::start(&config.run_dir, current_uid(), handler) {
            Ok(server) => Some(server),
            Err(e) => {
                logger.line(&format!(
                    "control socket unavailable ({e}); status and control are disabled"
                ));
                None
            }
        };
        let supervisor = Supervisor {
            provider: provider
                .as_ref()
                .map(|p| Box::new(p.provider()) as Box<dyn WifiContextProvider + Send>),
            config,
            shared,
            commands: rx,
            stop: stop.clone(),
            pair: Mutex::new(PairControl::default()),
        };
        let handle = thread::Builder::new()
            .name("supervisor".into())
            .spawn(move || supervisor.run())
            .map_err(StartError::Io)?;
        logger.line(&format!(
            "cyd-hostd {} started (pid {})",
            env!("CARGO_PKG_VERSION"),
            std::process::id()
        ));
        Ok(Daemon {
            _lock: lock,
            control,
            provider,
            stop,
            supervisor: Some(handle),
            logger,
        })
    }

    pub fn stop_flag(&self) -> Arc<AtomicBool> {
        self.stop.clone()
    }

    pub fn control_socket(&self) -> Option<PathBuf> {
        self.control.as_ref().map(|c| c.socket_path().to_path_buf())
    }

    pub fn provider_socket(&self) -> Option<PathBuf> {
        self.provider
            .as_ref()
            .map(|p| p.socket_path().to_path_buf())
    }

    pub fn is_running(&self) -> bool {
        self.supervisor.as_ref().is_some_and(|h| !h.is_finished())
    }

    /// Stops and joins everything; the lock is released last.
    pub fn shutdown(mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.supervisor.take() {
            let _ = handle.join();
        }
        self.control.take();
        self.provider.take();
        self.logger.line("cyd-hostd stopped");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct NoPorts;
    impl PortDiscovery for NoPorts {
        fn candidates(&self) -> Vec<String> {
            Vec::new()
        }
        fn exists(&self, _path: &str) -> bool {
            false
        }
    }

    fn handler(provider: Option<Arc<WifiContextServer>>) -> DaemonHandler {
        let tracker = provider.as_ref().map(|server| server.provider().0);
        DaemonHandler {
            shared: Arc::new(Mutex::new(Shared {
                link: LinkStatus {
                    state: LinkState::PairedConnected,
                    detail: Some("a \"quoted\"\tdetail\u{1}".into()),
                    port: Some("/dev/cu.usbserial-10".into()),
                    pairing_code: Some(4_242),
                    pairing_identity: Some("d0d0d0d0".into()),
                    sessions: 2,
                },
                last_snapshot_utc_ms: Some(1_790_205_982_430),
                // In the future of any clock: the reported age saturates to 0.
                last_snapshot_ms: Some(u64::MAX),
                snapshots: 88,
                groups: vec![("cpu", "available"), ("battery", "absent")],
            })),
            commands: Mutex::new(mpsc::channel().0),
            stop: Arc::new(AtomicBool::new(false)),
            provider,
            tracker,
            discovery: Arc::new(NoPorts),
            clock: Clock::new(),
            started_ms: u64::MAX, // uptime 0
            version: "1.2.3",
        }
    }

    fn control() -> ControlStats {
        ControlStats {
            accepted: 1,
            requests: 2,
            wrong_uid: 3,
            malformed: 4,
            oversized: 5,
            bad_version: 6,
            unknown_command: 7,
            busy: 8,
            closed_for_bad_lines: 9,
        }
    }

    /// The status reply is a documented contract read by the setup app: its
    /// bytes (key order, escaping, nulls) are pinned here.
    #[test]
    fn the_status_reply_is_byte_for_byte_stable() {
        let (pid, uid) = (std::process::id(), current_uid());
        let expected = |wifi: &str, provider_counters: &str| {
            format!(
                "{{\"daemon\":{{\"version\":\"1.2.3\",\"pid\":{pid},\"uptime_s\":0,\"uid\":{uid}}},\
                 \"link\":{{\"state\":\"paired_connected\",\"detail\":\"a \\\"quoted\\\"\\tdetail\\u0001\",\
                 \"port\":\"/dev/cu.usbserial-10\",\"pairing_identity\":\"d0d0d0d0\",\
                 \"pairing_code\":\"004242\",\"sessions\":2,\"snapshots\":88,\
                 \"last_snapshot_utc_ms\":1790205982430,\"last_snapshot_age_ms\":0}},\
                 \"metrics\":{{\"cpu\":\"available\",\"battery\":\"absent\"}},\
                 \"wifi_context\":{wifi},\
                 \"counters\":{{\"provider\":{provider_counters},\"control\":{{\"accepted\":1,\
                 \"requests\":2,\"wrong_uid\":3,\"malformed\":4,\"oversized\":5,\"bad_version\":6,\
                 \"unknown_command\":7,\"busy\":8,\"closed_for_bad_lines\":9}}}}}}"
            )
        };
        assert_eq!(
            handler(None).status(&control()),
            expected(
                "{\"provider\":\"server_unavailable\",\"availability\":\"not_requested\",\"permission\":null}",
                "null"
            )
        );

        let dir = std::env::temp_dir().join(format!("cyd-status-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let clock = Clock::new();
        let server = WifiContextServer::start(ServerConfig {
            dir: dir.clone(),
            expected_uid: current_uid(),
            now_ms: clock.now_fn(),
        })
        .unwrap();
        let status = handler(Some(Arc::new(server))).status(&control());
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(
            status,
            expected(
                "{\"provider\":\"never_connected\",\"availability\":\"not_requested\",\"permission\":null}",
                "{\"accepted\":0,\"replaced\":0,\"wrong_uid\":0,\"malformed\":0,\"oversized\":0,\
                 \"out_of_order\":0,\"rate_limited\":0,\"bad_version\":0,\"closed_for_bad_lines\":0,\
                 \"applied\":0}"
            )
        );
    }
}
