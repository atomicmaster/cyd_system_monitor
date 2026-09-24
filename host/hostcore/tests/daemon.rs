// SPDX-License-Identifier: GPL-3.0-only

//! Runs the real daemon logic against the real firmware `HostLink` (the
//! `link_cli` scripted device built by `./dev check firmware`), with sockets,
//! lock and identity in temp directories and an injectable clock. Nothing here
//! touches a real serial port, the user's directories or launchd.

use std::collections::VecDeque;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use hostcore::control::{request, simple_request};
use hostcore::daemon::{Clock, Daemon, DaemonConfig, PortDiscovery, PortOpener, StartError};
use hostcore::link::Channel;
use hostcore::logging::Logger;

const PORT: &str = "/dev/cu.usbserial-FAKE";

fn link_cli() -> PathBuf {
    if let Ok(path) = std::env::var("CYD_LINK_CLI") {
        return path.into();
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../firmware/domain/tests/build/link_cli")
}

struct DeviceProcess {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    inbox: VecDeque<u8>,
    paired: bool,
    seen: bool,
    phase: u32,
    pushes: u64,
}

impl Drop for DeviceProcess {
    fn drop(&mut self) {
        let _ = self.child.kill();
    }
}

fn from_hex(hex: &str) -> Vec<u8> {
    (0..hex.len() / 2)
        .map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

impl DeviceProcess {
    fn command(&mut self, line: &str) {
        writeln!(self.stdin, "{line}").unwrap();
        loop {
            let mut reply = String::new();
            assert!(
                self.stdout.read_line(&mut reply).unwrap() > 0,
                "link_cli exited"
            );
            let reply = reply.trim();
            if let Some(hex) = reply.strip_prefix("OUT ") {
                self.inbox.extend(from_hex(hex));
            } else if let Some(rest) = reply.strip_prefix("STATE ") {
                let field = |name: &str| {
                    rest.split_whitespace()
                        .find_map(|kv| kv.strip_prefix(&format!("{name}=")))
                        .unwrap()
                };
                self.paired = field("paired") == "1";
                self.seen = field("seen") == "1";
                self.phase = field("phase").parse().unwrap();
                return;
            }
        }
    }
}

/// The scripted display: one persistent firmware instance (its pairing
/// survives a replug, like the board's flash) that can be plugged in or out.
struct FakeDevice {
    process: Mutex<DeviceProcess>,
    plugged: AtomicBool,
    generation: AtomicU64,
}

impl FakeDevice {
    fn new() -> Arc<FakeDevice> {
        let path = link_cli();
        assert!(
            path.exists(),
            "{} is missing: run `./dev check firmware` (or set CYD_LINK_CLI) first",
            path.display()
        );
        let mut child = Command::new(path)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .expect("spawn link_cli");
        let stdin = child.stdin.take().unwrap();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        Arc::new(FakeDevice {
            process: Mutex::new(DeviceProcess {
                child,
                stdin,
                stdout,
                inbox: VecDeque::new(),
                paired: false,
                seen: false,
                phase: 0,
                pushes: 0,
            }),
            plugged: AtomicBool::new(false),
            generation: AtomicU64::new(0),
        })
    }

    fn plug(&self) {
        self.plugged.store(true, Ordering::SeqCst);
    }

    fn unplug(&self) {
        self.plugged.store(false, Ordering::SeqCst);
        self.generation.fetch_add(1, Ordering::SeqCst);
    }

    /// The touch confirmation on the display.
    fn confirm(&self, accept: bool) {
        self.process
            .lock()
            .unwrap()
            .command(&format!("CONFIRM {}", u8::from(accept)));
    }

    fn state(&self) -> (bool, bool, u64) {
        let p = self.process.lock().unwrap();
        (p.paired, p.seen, p.pushes)
    }
}

struct FakeChannel {
    device: Arc<FakeDevice>,
    generation: u64,
}

impl FakeChannel {
    fn check(&self) -> io::Result<()> {
        if self.device.generation.load(Ordering::SeqCst) != self.generation {
            return Err(io::Error::new(io::ErrorKind::BrokenPipe, "unplugged"));
        }
        Ok(())
    }
}

impl Channel for FakeChannel {
    fn read_timeout(&mut self, buf: &mut [u8], timeout: Duration) -> io::Result<usize> {
        self.check()?;
        let mut p = self.device.process.lock().unwrap();
        if p.inbox.is_empty() {
            drop(p);
            std::thread::sleep(timeout.min(Duration::from_millis(20)));
            return Ok(0);
        }
        let n = buf.len().min(p.inbox.len());
        for slot in buf.iter_mut().take(n) {
            *slot = p.inbox.pop_front().unwrap();
        }
        Ok(n)
    }

    fn write_all(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.check()?;
        if bytes.starts_with(b"CYD:LINK") {
            return Ok(()); // the console line, not a frame
        }
        let mut p = self.device.process.lock().unwrap();
        p.pushes += 1;
        p.command(&format!("PUSH {}", to_hex(bytes)));
        Ok(())
    }
}

/// A handle the daemon config can own.
#[derive(Clone)]
struct Handle(Arc<FakeDevice>);

impl PortOpener for Handle {
    fn open(&self, _path: &str) -> io::Result<Box<dyn Channel>> {
        if !self.0.plugged.load(Ordering::SeqCst) {
            return Err(io::Error::new(io::ErrorKind::NotFound, "no such device"));
        }
        Ok(Box::new(FakeChannel {
            device: self.0.clone(),
            generation: self.0.generation.load(Ordering::SeqCst),
        }))
    }
}

impl PortDiscovery for Handle {
    fn candidates(&self) -> Vec<String> {
        if self.0.plugged.load(Ordering::SeqCst) {
            vec![PORT.into()]
        } else {
            vec![]
        }
    }

    fn exists(&self, path: &str) -> bool {
        self.0.plugged.load(Ordering::SeqCst) && path == PORT
    }
}

struct Rig {
    dir: PathBuf,
    device: Arc<FakeDevice>,
    clock: Clock,
    daemon: Option<Daemon>,
}

fn temp_dir(name: &str) -> PathBuf {
    // `sun_path` is short on macOS: keep the run directory path small.
    let dir = std::path::Path::new("/tmp").join(format!("cyd-it-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn config(dir: &Path, device: &Arc<FakeDevice>, clock: &Clock) -> DaemonConfig {
    DaemonConfig {
        run_dir: dir.join("run"),
        state_dir: dir.join("state"),
        port: None,
        opener: Arc::new(Handle(device.clone())),
        discovery: Arc::new(Handle(device.clone())),
        clock: clock.clone(),
        boot_wait: Duration::from_millis(10),
        retry_wait: Duration::from_millis(50),
        handshake_deadline: Duration::from_secs(20),
        logger: Arc::new(Logger::Null),
    }
}

impl Rig {
    fn new(name: &str) -> Rig {
        let dir = temp_dir(name);
        let device = FakeDevice::new();
        let clock = Clock::new();
        let daemon = Daemon::start(config(&dir, &device, &clock)).expect("daemon starts");
        Rig {
            dir,
            device,
            clock,
            daemon: Some(daemon),
        }
    }

    fn run_dir(&self) -> PathBuf {
        self.dir.join("run")
    }

    fn ask(&self, line: &str) -> String {
        request(&self.run_dir(), line, Duration::from_secs(10)).expect("daemon answers")
    }

    fn status(&self) -> String {
        self.ask(&simple_request("status"))
    }

    /// Polls `status` until `check` accepts it.
    fn wait_status(&self, what: &str, check: impl Fn(&str) -> bool) -> String {
        let end = Instant::now() + Duration::from_secs(15);
        loop {
            let status = self.status();
            if check(&status) {
                return status;
            }
            assert!(
                Instant::now() < end,
                "timed out waiting for {what}; last status: {status}"
            );
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    fn pair(&self) {
        self.device.plug();
        self.wait_status("pairing_required", |s| {
            field(s, "state") == "pairing_required"
        });
        assert!(self
            .ask(&simple_request("pair_start"))
            .contains("\"ok\":true"));
        self.wait_status("a matching code", |s| field(s, "pairing_code") != "null");
        self.device.confirm(true);
        self.wait_status("paired_connected", |s| {
            field(s, "state") == "paired_connected"
        });
    }
}

impl Drop for Rig {
    fn drop(&mut self) {
        if let Some(daemon) = self.daemon.take() {
            daemon.shutdown();
        }
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

/// The raw JSON value after `"key":` (strings unquoted), `""` when absent.
fn field<'a>(json: &'a str, key: &str) -> &'a str {
    let Some(at) = json.find(&format!("\"{key}\":")) else {
        return "";
    };
    let rest = &json[at + key.len() + 3..];
    let end = rest.find([',', '}']).unwrap_or(rest.len());
    rest[..end].trim_matches('"')
}

fn number(json: &str, key: &str) -> u64 {
    field(json, key)
        .parse()
        .unwrap_or_else(|_| panic!("{key} in {json}"))
}

#[test]
fn status_reports_every_link_state_from_no_device_to_paired() {
    let rig = Rig::new("status");
    let status = rig.status();
    assert!(status.contains("\"ok\":true"), "{status}");
    assert_eq!(field(&status, "state"), "no_device");
    assert_eq!(number(&status, "pid"), u64::from(std::process::id()));
    assert_eq!(field(&status, "provider"), "never_connected");
    assert_eq!(field(&status, "availability"), "not_requested");
    assert_eq!(field(&status, "permission"), "null");
    assert_eq!(field(&status, "pairing_identity"), "null");
    assert!(status.contains("\"metrics\":{}"), "{status}");
    assert_eq!(
        rig.ask(&simple_request("monitors")),
        "{\"v\":1,\"ok\":true,\"cmd\":\"monitors\",\"monitors\":[]}"
    );

    // A display appears but this Mac is not paired with it: it is not touched
    // until pairing is asked for.
    rig.device.plug();
    let status = rig.wait_status("pairing_required", |s| {
        field(s, "state") == "pairing_required"
    });
    assert_eq!(field(&status, "port"), PORT);
    assert_eq!(number(&status, "sessions"), 0);
    assert!(rig.ask(&simple_request("monitors")).contains(PORT));

    assert!(rig
        .ask(&simple_request("pair_start"))
        .contains("\"ok\":true"));
    let status = rig.wait_status("a matching code", |s| field(s, "pairing_code") != "null");
    assert_eq!(field(&status, "pairing_code").len(), 6);
    assert_eq!(field(&status, "state"), "pairing_required");

    rig.device.confirm(true);
    let status = rig.wait_status("snapshots", |s| {
        field(s, "state") == "paired_connected" && field(s, "snapshots") != "0"
    });
    assert_eq!(field(&status, "pairing_identity").len(), 8);
    assert_eq!(field(&status, "pairing_code"), "null");
    for group in ["cpu", "ram", "storage", "agents", "network"] {
        assert!(
            status.contains(&format!("\"{group}\":\"")),
            "{group} in {status}"
        );
    }
    assert_ne!(field(&status, "last_snapshot_utc_ms"), "null");
    assert!(number(&status, "last_snapshot_age_ms") < 5_000);
    let (paired, seen, _) = rig.device.state();
    assert!(paired && seen, "the display saw a snapshot");

    // Pairing again is refused while paired.
    assert!(rig
        .ask(&simple_request("pair_start"))
        .contains("already_paired"));
}

#[test]
fn a_second_daemon_is_refused_while_the_first_runs_and_allowed_after() {
    let rig = Rig::new("second");
    let device = FakeDevice::new();
    let clock = Clock::new();
    match Daemon::start(config(&rig.dir, &device, &clock)) {
        Err(StartError::AlreadyRunning { pid }) => {
            assert_eq!(pid, Some(std::process::id()));
            assert!(StartError::AlreadyRunning { pid }
                .to_string()
                .contains("already owns the display's serial port"));
        }
        Err(other) => panic!("unexpected error {other}"),
        Ok(_) => panic!("a second daemon must be refused"),
    }
    // The first is undisturbed.
    assert!(rig.status().contains("\"ok\":true"));
    let mut rig = rig;
    rig.daemon.take().unwrap().shutdown();
    assert!(Daemon::start(config(&rig.dir, &device, &clock))
        .map(Daemon::shutdown)
        .is_ok());
}

fn send_provider(stream: &mut UnixStream, lines: &[&str]) {
    for line in lines {
        stream.write_all(line.as_bytes()).unwrap();
        stream.write_all(b"\n").unwrap();
    }
}

const HELLO: &str = r#"{"v":1,"type":"hello","provider":"it","provider_version":"1"}"#;
const PERMISSION: &str = r#"{"v":1,"type":"permission_status","seq":1,"status":"granted"}"#;
const CONTEXT: &str = r#"{"v":1,"type":"context","seq":2,"t_ms":1000,"age_ms":0,"availability":"available","band":"5","channel":36,"ssid_hex":"74657374","bssid":"aa:bb:cc:dd:ee:ff"}"#;

#[test]
fn provider_context_degrades_to_error_after_15_s_of_silence_or_disconnect() {
    let rig = Rig::new("provider");
    let provider_socket = rig.run_dir().join("wifi-context.sock");

    // Silent but connected provider.
    let mut stream = UnixStream::connect(&provider_socket).unwrap();
    send_provider(&mut stream, &[HELLO, PERMISSION, CONTEXT]);
    let status = rig.wait_status("available context", |s| {
        field(s, "availability") == "available"
    });
    assert_eq!(field(&status, "provider"), "connected");
    assert_eq!(field(&status, "permission"), "granted");
    rig.clock.advance(14_000);
    assert_eq!(
        field(&rig.status(), "availability"),
        "available",
        "still fresh at 14 s"
    );
    rig.clock.advance(2_000);
    let status = rig.status();
    assert_eq!(
        field(&status, "availability"),
        "error",
        "stale at 16 s: {status}"
    );
    assert_eq!(field(&status, "provider"), "connected");
    assert_eq!(
        field(&status, "permission"),
        "granted",
        "last reported state kept"
    );
    // The provider socket receives on the daemon's own (sleep-inclusive)
    // clock, so a context sent after the jump is fresh again.
    send_provider(
        &mut stream,
        &[CONTEXT.replace("\"seq\":2", "\"seq\":3").as_str()],
    );
    rig.wait_status("fresh context after the jump", |s| {
        field(s, "availability") == "available"
    });

    // A provider that goes away: the value ages out the same way, and the
    // daemon carries on.
    let rig2 = Rig::new("provider2");
    let mut stream = UnixStream::connect(rig2.run_dir().join("wifi-context.sock")).unwrap();
    send_provider(&mut stream, &[HELLO, PERMISSION, CONTEXT]);
    rig2.wait_status("available context", |s| {
        field(s, "availability") == "available"
    });
    drop(stream);
    let status = rig2.wait_status("disconnect", |s| field(s, "provider") == "disconnected");
    assert_eq!(field(&status, "availability"), "available");
    rig2.clock.advance(16_000);
    assert_eq!(field(&rig2.status(), "availability"), "error");
    assert!(rig2.ask(&simple_request("status")).contains("\"ok\":true"));

    // A provider that never spoke a valid line is counted, not fatal.
    let mut junk = UnixStream::connect(&provider_socket).unwrap();
    send_provider(&mut junk, &["garbage"]);
    rig.wait_status("malformed counted", |s| field(s, "malformed") != "0");
}

#[test]
fn closing_and_reopening_status_clients_does_not_disturb_the_snapshot_loop() {
    let rig = Rig::new("clients");
    rig.pair();
    let first = rig.status();
    let (sessions, snapshots) = (number(&first, "sessions"), number(&first, "snapshots"));
    assert_eq!(sessions, 1);
    for round in 0..30 {
        let mut stream = UnixStream::connect(rig.run_dir().join("control.sock")).unwrap();
        match round % 3 {
            0 => {}                                                    // connect and hang up at once
            1 => stream.write_all(b"{\"v\":1,\"cmd\":\"sta").unwrap(), // half a request
            _ => {
                stream
                    .write_all(simple_request("status").as_bytes())
                    .unwrap();
                stream.write_all(b"\n").unwrap();
                let mut byte = [0u8; 1];
                let _ = stream.read(&mut byte); // read part of the reply, then vanish
            }
        }
    }
    let status = rig.wait_status("further snapshots", |s| {
        number(s, "snapshots") >= snapshots + 3
    });
    assert_eq!(
        number(&status, "sessions"),
        1,
        "no reconnect, no second serial owner"
    );
    assert_eq!(field(&status, "state"), "paired_connected");
}

#[test]
fn unplug_and_replug_resynchronizes_with_a_fresh_session() {
    let rig = Rig::new("replug");
    rig.pair();
    assert_eq!(number(&rig.status(), "sessions"), 1);

    rig.device.unplug();
    let status = rig.wait_status("the loss", |s| field(s, "state") == "no_device");
    assert!(
        status.contains("\"metrics\":{}"),
        "no stale group summary: {status}"
    );
    assert_ne!(
        field(&status, "pairing_identity"),
        "null",
        "the pairing is kept"
    );
    let (_, _, pushes_before) = rig.device.state();

    rig.device.plug();
    let status = rig.wait_status("resync", |s| {
        field(s, "state") == "paired_connected" && number(s, "sessions") == 2
    });
    // No touch was needed (known display) and complete snapshots resumed.
    assert_eq!(field(&status, "pairing_code"), "null");
    rig.wait_status("snapshots on the new session", |s| {
        number(s, "snapshots") > number(&status, "snapshots") + 1
    });
    let (paired, seen, pushes_after) = rig.device.state();
    assert!(paired && seen);
    assert!(
        pushes_after >= pushes_before + 3,
        "hello, pair and snapshots"
    );
}

#[test]
fn sleep_wake_resynchronizes_with_a_fresh_session() {
    let rig = Rig::new("wake");
    rig.pair();
    let before = rig.status();
    assert_eq!(number(&before, "sessions"), 1);
    rig.clock.advance(30_000); // the Mac slept for 30 s
    let status = rig.wait_status("resync after wake", |s| {
        field(s, "state") == "paired_connected" && number(s, "sessions") == 2
    });
    assert_eq!(field(&status, "pairing_code"), "null");
    rig.wait_status("snapshots again", |s| {
        number(s, "snapshots") > number(&status, "snapshots")
    });
}

#[test]
fn unpair_is_explicit_and_forgets_only_the_stored_display() {
    let rig = Rig::new("unpair");
    rig.pair();
    let identity_file = rig.dir.join("state/identity");
    assert!(std::fs::read_to_string(&identity_file)
        .unwrap()
        .contains("device="));
    let reply = rig.ask(&simple_request("unpair"));
    assert!(reply.contains("\"ok\":true"), "{reply}");
    let status = rig.wait_status("unpaired", |s| field(s, "state") == "pairing_required");
    assert_eq!(field(&status, "pairing_identity"), "null");
    let text = std::fs::read_to_string(&identity_file).unwrap();
    assert!(
        text.contains("host=") && !text.contains("device="),
        "host identity kept"
    );
    // Not reconnected on its own; pairing needs an explicit request again.
    std::thread::sleep(Duration::from_millis(600));
    assert_eq!(field(&rig.status(), "sessions"), "1");
    // Unpair with no live link says so.
    assert!(rig.ask(&simple_request("unpair")).contains("not_connected"));
}

#[test]
fn the_stop_command_ends_the_daemon_cleanly_and_releases_everything() {
    let mut rig = Rig::new("stop");
    let daemon = rig.daemon.as_ref().unwrap();
    assert!(daemon.is_running());
    assert!(rig.ask(&simple_request("stop")).contains("\"ok\":true"));
    let end = Instant::now() + Duration::from_secs(5);
    while rig.daemon.as_ref().unwrap().is_running() {
        assert!(Instant::now() < end, "the supervisor did not stop");
        std::thread::sleep(Duration::from_millis(20));
    }
    rig.daemon.take().unwrap().shutdown();
    assert!(!rig.run_dir().join("control.sock").exists());
    assert!(!rig.run_dir().join("wifi-context.sock").exists());
    assert!(!hostcore::lock::InstanceLock::is_held(&rig.run_dir()));
}
