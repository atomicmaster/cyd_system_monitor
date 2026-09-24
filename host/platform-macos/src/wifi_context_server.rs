// SPDX-License-Identifier: GPL-3.0-only

//! The daemon side of the WiFi context provider socket
//! (`docs/wifi-context-provider.md`).
//!
//! The daemon owns a Unix domain socket in a per-user directory (0700, socket
//! 0600). One same-user provider connects and sends bounded JSON lines; the
//! newest connection replaces the previous one. Everything malformed,
//! oversized, out of order, too fast or from another uid is dropped and
//! counted; nothing a provider sends can stall or crash the daemon, and the
//! daemon keeps monitoring whatever the provider does. The parsed state lives
//! in a [`ContextTracker`] that the network collector reads.

use std::collections::VecDeque;
use std::ffi::c_int;
use std::fs::{self, DirBuilder};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, PermissionsExt};
use std::os::unix::io::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::time::NowMs;
use metrics::wifi_context::{
    parse_provider_message, ContextTracker, MessageError, ProviderMessage, TrackerProvider,
    IPC_VERSION, MAX_LINE_BYTES,
};

/// The socket's file name inside the run directory.
pub const SOCKET_NAME: &str = "wifi-context.sock";
/// `sizeof(sockaddr_un.sun_path)` on Darwin; the path plus NUL must fit.
const MAX_SOCKET_PATH: usize = 104;
/// Most messages accepted from one provider in any second. The contract asks
/// for a change-driven message plus one heartbeat per second.
const MAX_MESSAGES_PER_SECOND: usize = 20;
/// Bad lines in a row before the connection is dropped as a broken provider.
const MAX_CONSECUTIVE_BAD: u32 = 16;
const POLL: Duration = Duration::from_millis(20);

extern "C" {
    fn getpeereid(fd: c_int, euid: *mut u32, egid: *mut u32) -> c_int;
    fn geteuid() -> u32;
}

/// The calling user's effective uid.
pub fn current_uid() -> u32 {
    // SAFETY: no arguments, cannot fail.
    unsafe { geteuid() }
}

pub fn peer_uid(stream: &UnixStream) -> Option<u32> {
    let (mut uid, mut gid) = (0u32, 0u32);
    // SAFETY: a valid socket fd and valid out pointers.
    (unsafe { getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) } == 0).then_some(uid)
}

#[derive(Clone)]
pub struct ServerConfig {
    /// The per-user run directory; created 0700 if missing.
    pub dir: PathBuf,
    /// The only uid whose connections are accepted: the daemon's own
    /// ([`current_uid`]); tests pass another to exercise the refusal.
    pub expected_uid: u32,
    /// The monotonic clock the collector uses (it counts sleep), so context
    /// ages line up and a context from before a sleep is as old as the sleep.
    pub now_ms: NowMs,
}

/// What the server has refused or dropped since it started.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Stats {
    pub accepted: u64,
    pub replaced: u64,
    pub wrong_uid: u64,
    pub malformed: u64,
    pub oversized: u64,
    pub out_of_order: u64,
    pub rate_limited: u64,
    pub bad_version: u64,
    pub closed_for_bad_lines: u64,
    pub applied: u64,
}

#[derive(Default)]
struct Counters {
    accepted: AtomicU64,
    replaced: AtomicU64,
    wrong_uid: AtomicU64,
    malformed: AtomicU64,
    oversized: AtomicU64,
    out_of_order: AtomicU64,
    rate_limited: AtomicU64,
    bad_version: AtomicU64,
    closed_for_bad_lines: AtomicU64,
    applied: AtomicU64,
}

fn bump(counter: &AtomicU64) {
    counter.fetch_add(1, Ordering::Relaxed);
}

pub struct WifiContextServer {
    path: PathBuf,
    tracker: Arc<Mutex<ContextTracker>>,
    counters: Arc<Counters>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

/// Creates (or tightens) the run directory: real directory, owned by the
/// caller, no group or other access.
pub fn prepare_dir(dir: &Path, uid: u32) -> io::Result<()> {
    DirBuilder::new().recursive(true).mode(0o700).create(dir)?;
    let meta = fs::symlink_metadata(dir)?;
    if !meta.file_type().is_dir() {
        return Err(io::Error::other(
            "the run directory path is not a directory",
        ));
    }
    if meta.uid() != uid {
        return Err(io::Error::other(
            "the run directory belongs to another user",
        ));
    }
    if meta.mode() & 0o077 != 0 {
        fs::set_permissions(dir, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

/// Removes a leftover socket from a crashed daemon; refuses when the path is
/// something else or a live daemon still answers on it.
pub fn clear_stale_socket(path: &Path, uid: u32) -> io::Result<()> {
    let meta = match fs::symlink_metadata(path) {
        Ok(meta) => meta,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e),
    };
    if !meta.file_type().is_socket() || meta.uid() != uid {
        return Err(io::Error::other(
            "the socket path is occupied by something else",
        ));
    }
    if UnixStream::connect(path).is_ok() {
        return Err(io::Error::new(
            io::ErrorKind::AddrInUse,
            "another daemon already owns the WiFi context socket",
        ));
    }
    fs::remove_file(path)
}

impl WifiContextServer {
    pub fn start(config: ServerConfig) -> io::Result<Self> {
        let path = config.dir.join(SOCKET_NAME);
        if path.as_os_str().len() >= MAX_SOCKET_PATH {
            return Err(io::Error::other("the socket path is too long for sun_path"));
        }
        prepare_dir(&config.dir, current_uid())?;
        clear_stale_socket(&path, current_uid())?;
        let listener = UnixListener::bind(&path)?;
        // The 0700 directory already keeps everyone else out; this is the
        // documented file mode as well.
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
        listener.set_nonblocking(true)?;
        let tracker = Arc::new(Mutex::new(ContextTracker::default()));
        let counters = Arc::new(Counters::default());
        let stop = Arc::new(AtomicBool::new(false));
        let thread = {
            let (tracker, counters, stop) = (tracker.clone(), counters.clone(), stop.clone());
            thread::Builder::new()
                .name("wifi-context".into())
                .spawn(move || serve(listener, config, tracker, counters, stop))?
        };
        Ok(Self {
            path,
            tracker,
            counters,
            stop,
            thread: Some(thread),
        })
    }

    pub fn socket_path(&self) -> &Path {
        &self.path
    }

    /// The provider for the network collector.
    pub fn provider(&self) -> TrackerProvider {
        TrackerProvider(self.tracker.clone())
    }

    pub fn stats(&self) -> Stats {
        let c = &self.counters;
        let get = |a: &AtomicU64| a.load(Ordering::Relaxed);
        Stats {
            accepted: get(&c.accepted),
            replaced: get(&c.replaced),
            wrong_uid: get(&c.wrong_uid),
            malformed: get(&c.malformed),
            oversized: get(&c.oversized),
            out_of_order: get(&c.out_of_order),
            rate_limited: get(&c.rate_limited),
            bad_version: get(&c.bad_version),
            closed_for_bad_lines: get(&c.closed_for_bad_lines),
            applied: get(&c.applied),
        }
    }
}

impl Drop for WifiContextServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        let _ = fs::remove_file(&self.path);
    }
}

/// One provider connection's parsing state.
struct Connection {
    stream: UnixStream,
    line: Vec<u8>,
    discarding: bool,
    hello_seen: bool,
    last_seq: Option<u64>,
    last_t_ms: u64,
    consecutive_bad: u32,
    recent: VecDeque<Instant>,
}

enum Verdict {
    Keep,
    Close,
}

fn serve(
    listener: UnixListener,
    config: ServerConfig,
    tracker: Arc<Mutex<ContextTracker>>,
    counters: Arc<Counters>,
    stop: Arc<AtomicBool>,
) {
    let mut current: Option<Connection> = None;
    let mut buf = [0u8; 1024];
    while !stop.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, _)) => {
                if peer_uid(&stream) != Some(config.expected_uid) {
                    bump(&counters.wrong_uid);
                    drop(stream); // closed without a byte read or written
                } else if stream.set_read_timeout(Some(POLL)).is_ok()
                    && stream
                        .set_write_timeout(Some(Duration::from_millis(200)))
                        .is_ok()
                {
                    bump(&counters.accepted);
                    if let Some(old) = current.take() {
                        bump(&counters.replaced);
                        close(old, &tracker);
                    }
                    current = Some(Connection {
                        stream,
                        line: Vec::new(),
                        discarding: false,
                        hello_seen: false,
                        last_seq: None,
                        last_t_ms: 0,
                        consecutive_bad: 0,
                        recent: VecDeque::new(),
                    });
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {}
            Err(_) => thread::sleep(POLL),
        }
        let Some(conn) = current.as_mut() else {
            thread::sleep(POLL);
            continue;
        };
        match conn.stream.read(&mut buf) {
            Ok(0) => close_current(&mut current, &tracker),
            Ok(n) => {
                let mut verdict = Verdict::Keep;
                for &byte in &buf[..n] {
                    if let Verdict::Close = feed(conn, byte, &tracker, &counters, &config.now_ms) {
                        verdict = Verdict::Close;
                        break;
                    }
                }
                if let Verdict::Close = verdict {
                    close_current(&mut current, &tracker);
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) => {}
            Err(_) => close_current(&mut current, &tracker),
        }
    }
    if let Some(conn) = current.take() {
        close(conn, &tracker);
    }
}

fn close_current(current: &mut Option<Connection>, tracker: &Arc<Mutex<ContextTracker>>) {
    if let Some(conn) = current.take() {
        close(conn, tracker);
    }
}

fn close(conn: Connection, tracker: &Arc<Mutex<ContextTracker>>) {
    if conn.hello_seen {
        tracker
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .on_disconnected();
    }
    let _ = conn.stream.shutdown(std::net::Shutdown::Both);
}

/// Adds one byte to the current line and handles the line when it ends.
fn feed(
    conn: &mut Connection,
    byte: u8,
    tracker: &Arc<Mutex<ContextTracker>>,
    counters: &Counters,
    now_ms: &NowMs,
) -> Verdict {
    if byte != b'\n' {
        if conn.discarding {
            return Verdict::Keep;
        }
        if conn.line.len() >= MAX_LINE_BYTES {
            // Over the bound: drop the rest of this line, never buffer it.
            conn.discarding = true;
            conn.line.clear();
            bump(&counters.oversized);
            return bad_line(conn, counters);
        }
        conn.line.push(byte);
        return Verdict::Keep;
    }
    if conn.discarding {
        conn.discarding = false;
        return Verdict::Keep;
    }
    let line = std::mem::take(&mut conn.line);
    if line.is_empty() {
        return Verdict::Keep; // a blank line is harmless
    }
    handle_line(conn, &line, tracker, counters, now_ms)
}

fn bad_line(conn: &mut Connection, counters: &Counters) -> Verdict {
    conn.consecutive_bad += 1;
    if conn.consecutive_bad >= MAX_CONSECUTIVE_BAD {
        bump(&counters.closed_for_bad_lines);
        Verdict::Close
    } else {
        Verdict::Keep
    }
}

fn handle_line(
    conn: &mut Connection,
    line: &[u8],
    tracker: &Arc<Mutex<ContextTracker>>,
    counters: &Counters,
    now_ms: &NowMs,
) -> Verdict {
    let now = Instant::now();
    while conn
        .recent
        .front()
        .is_some_and(|t| now.duration_since(*t) >= Duration::from_secs(1))
    {
        conn.recent.pop_front();
    }
    if conn.recent.len() >= MAX_MESSAGES_PER_SECOND {
        bump(&counters.rate_limited);
        return Verdict::Keep;
    }
    conn.recent.push_back(now);
    let message = match parse_provider_message(line) {
        Ok(message) => message,
        Err(MessageError::BadVersion) => {
            bump(&counters.bad_version);
            let reply = format!(
                "{{\"v\":{IPC_VERSION},\"type\":\"error\",\"reason\":\"unsupported_version\"}}\n"
            );
            let _ = conn.stream.write_all(reply.as_bytes());
            return Verdict::Close;
        }
        Err(_) => {
            bump(&counters.malformed);
            return bad_line(conn, counters);
        }
    };
    // Sequence numbers rise across every message of a connection, and the
    // provider's own clock never runs backwards.
    let ordered = match &message {
        ProviderMessage::Hello { .. } => true,
        ProviderMessage::PermissionStatus { seq, .. } => {
            conn.last_seq.is_none_or(|last| *seq > last)
        }
        ProviderMessage::Context { seq, t_ms, .. } => {
            conn.last_seq.is_none_or(|last| *seq > last) && *t_ms >= conn.last_t_ms
        }
    };
    let first_is_hello = matches!(message, ProviderMessage::Hello { .. }) != conn.hello_seen;
    if !first_is_hello {
        // Data before hello, or a second hello.
        bump(&counters.malformed);
        return bad_line(conn, counters);
    }
    if !ordered {
        bump(&counters.out_of_order);
        return bad_line(conn, counters);
    }
    conn.consecutive_bad = 0;
    let mut t = tracker.lock().unwrap_or_else(|p| p.into_inner());
    match message {
        ProviderMessage::Hello { .. } => {
            conn.hello_seen = true;
            t.on_connected();
            drop(t);
            let ack = format!(
                "{{\"v\":{IPC_VERSION},\"type\":\"hello_ack\",\"heartbeat_max_ms\":1000,\"silence_limit_ms\":15000}}\n"
            );
            if conn.stream.write_all(ack.as_bytes()).is_err() {
                return Verdict::Close;
            }
        }
        ProviderMessage::PermissionStatus { seq, status } => {
            conn.last_seq = Some(seq);
            t.on_permission(status);
            bump(&counters.applied);
        }
        ProviderMessage::Context {
            seq,
            t_ms,
            age_ms,
            context,
        } => {
            conn.last_seq = Some(seq);
            conn.last_t_ms = t_ms;
            t.on_context(context, age_ms, now_ms());
            bump(&counters.applied);
        }
    }
    Verdict::Keep
}

#[cfg(test)]
mod tests {
    use super::*;
    use metrics::wifi_context::{ContextAvailability, WifiContextProvider};
    use std::io::{BufRead, BufReader};
    use std::sync::atomic::AtomicUsize;

    static NEXT: AtomicUsize = AtomicUsize::new(0);

    fn temp_dir() -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "cyd-wc-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    /// A server on a test clock that stands still at 0 unless moved.
    fn config(dir: &Path) -> ServerConfig {
        config_with_clock(dir, Arc::new(AtomicU64::new(0)))
    }

    fn config_with_clock(dir: &Path, clock: Arc<AtomicU64>) -> ServerConfig {
        ServerConfig {
            dir: dir.to_path_buf(),
            expected_uid: current_uid(),
            now_ms: Arc::new(move || clock.load(Ordering::SeqCst)),
        }
    }

    fn wait_until(what: &str, mut condition: impl FnMut() -> bool) {
        let start = Instant::now();
        while !condition() {
            assert!(
                start.elapsed() < Duration::from_secs(5),
                "timed out: {what}"
            );
            thread::sleep(Duration::from_millis(5));
        }
    }

    const HELLO: &str =
        r#"{"v":1,"type":"hello","provider":"test-provider","provider_version":"1"}"#;

    fn context(seq: u64, t_ms: u64, availability: &str, channel: &str, ssid: &str) -> String {
        format!(
            r#"{{"v":1,"type":"context","seq":{seq},"t_ms":{t_ms},"age_ms":0,"availability":"{availability}","band":"2.4","channel":{channel},"ssid_hex":{ssid},"bssid":null}}"#
        )
    }

    struct Client(UnixStream);
    impl Client {
        fn connect(server: &WifiContextServer) -> Self {
            let s = UnixStream::connect(server.socket_path()).unwrap();
            s.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            Client(s)
        }
        fn send(&mut self, line: &str) {
            let _ = self.0.write_all(line.as_bytes());
            let _ = self.0.write_all(b"\n");
        }
        fn hello(mut self) -> Self {
            self.send(HELLO);
            let mut ack = String::new();
            BufReader::new(self.0.try_clone().unwrap())
                .read_line(&mut ack)
                .unwrap();
            assert!(ack.contains("hello_ack"), "{ack}");
            self
        }
        fn is_closed_by_peer(&mut self) -> bool {
            let mut b = [0u8; 16];
            matches!(self.0.read(&mut b), Ok(0) | Err(_))
        }
    }

    fn availability_now(server: &WifiContextServer) -> ContextAvailability {
        let now = server_now(server);
        server.provider().latest(now).context.availability
    }
    fn server_now(_: &WifiContextServer) -> u64 {
        0 // the clock of `config`
    }

    #[test]
    fn the_socket_is_private_and_the_directory_is_tightened() {
        let dir = temp_dir();
        fs::create_dir_all(&dir).unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).unwrap();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mode = |p: &Path| fs::metadata(p).unwrap().mode() & 0o777;
        assert_eq!(mode(&dir), 0o700);
        assert_eq!(mode(server.socket_path()), 0o600);
        drop(server);
        assert!(
            !dir.join(SOCKET_NAME).exists(),
            "the socket is removed on exit"
        );
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn nothing_connected_reports_permission_not_requested() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        assert_eq!(
            availability_now(&server),
            ContextAvailability::PermissionNotRequested
        );
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_provider_context_reaches_the_collector_and_a_disconnect_keeps_it_only_briefly() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut client = Client::connect(&server).hello();
        client.send(&context(1, 100, "available", "6", r#""4359442d54657374""#));
        wait_until("context applied", || server.stats().applied == 1);
        let reading = server.provider().latest(server_now(&server) + 50);
        assert_eq!(reading.context.availability, ContextAvailability::Available);
        assert_eq!(reading.context.channel, Some(6));
        assert_eq!(reading.context.ssid.as_deref(), Some(&b"CYD-Test"[..]));
        drop(client);
        // After the provider goes away the values age out at 15 s (tracker
        // tests cover the exact rule); here 20 s later they are a reason.
        wait_until("disconnect noticed", || {
            server
                .provider()
                .latest(server_now(&server) + 20_000)
                .context
                .availability
                == ContextAvailability::Error
        });
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_context_received_before_a_sleep_is_as_old_as_the_sleep_and_is_dropped() {
        let dir = temp_dir();
        let clock = Arc::new(AtomicU64::new(50_000));
        let server = WifiContextServer::start(config_with_clock(&dir, clock.clone())).unwrap();
        let mut client = Client::connect(&server).hello();
        client.send(&context(1, 100, "available", "6", r#""4359442d54657374""#));
        wait_until("context applied", || server.stats().applied == 1);
        let mut provider = server.provider();
        clock.store(51_000, Ordering::SeqCst);
        let fresh = provider.latest(clock.load(Ordering::SeqCst));
        assert_eq!(fresh.context.availability, ContextAvailability::Available);
        assert_eq!(fresh.age_ms, 1_000);
        // The Mac sleeps for two hours with the provider still connected: the
        // daemon's clock counts the sleep, so the context is 2 h old on wake
        // and is dropped (15 s limit), never shown as a seconds-old context.
        clock.store(51_000 + 2 * 3_600_000, Ordering::SeqCst);
        let woke = provider.latest(clock.load(Ordering::SeqCst));
        assert_eq!(woke.context.availability, ContextAvailability::Error);
        assert!(woke.context.ssid.is_none() && woke.context.channel.is_none());
        // The provider's fresh context on wake is reported again.
        client.send(&context(2, 200, "available", "11", "null"));
        wait_until("wake context applied", || server.stats().applied == 2);
        let again = provider.latest(clock.load(Ordering::SeqCst));
        assert_eq!(
            (
                again.context.availability,
                again.context.channel,
                again.age_ms
            ),
            (ContextAvailability::Available, Some(11), 0)
        );
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn malformed_oversized_and_out_of_order_input_is_dropped_and_counted() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut client = Client::connect(&server).hello();
        client.send("not json");
        client.send(r#"{"v":1,"type":"context"}"#);
        client.send(&"x".repeat(MAX_LINE_BYTES + 200)); // one oversized line
        client.send(&context(5, 500, "available", "1", "null"));
        client.send(&context(4, 600, "available", "11", "null")); // seq went back
        client.send(&context(6, 400, "available", "11", "null")); // clock went back
        client.send(HELLO); // a second hello
        client.send(&context(7, 700, "denied", "null", "null"));
        wait_until("all lines handled", || server.stats().applied == 2);
        let s = server.stats();
        assert_eq!((s.malformed, s.oversized, s.out_of_order), (3, 1, 2));
        // The good messages around the bad ones took effect.
        let now = server_now(&server);
        assert_eq!(
            server.provider().latest(now).context.availability,
            ContextAvailability::Denied
        );
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn data_before_hello_is_refused() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut client = Client::connect(&server);
        client.send(&context(1, 1, "available", "6", "null"));
        wait_until("refused", || server.stats().malformed == 1);
        assert_eq!(server.stats().applied, 0);
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn an_unsupported_version_is_answered_and_closed() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut client = Client::connect(&server);
        client.send(r#"{"v":2,"type":"hello","provider":"future","provider_version":"9"}"#);
        let mut reply = String::new();
        BufReader::new(client.0.try_clone().unwrap())
            .read_line(&mut reply)
            .unwrap();
        assert!(reply.contains("unsupported_version"), "{reply}");
        assert!(client.is_closed_by_peer());
        assert_eq!(server.stats().bad_version, 1);
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_connection_from_another_uid_is_dropped_and_counted() {
        let dir = temp_dir();
        let mut cfg = config(&dir);
        cfg.expected_uid = current_uid().wrapping_add(1);
        let server = WifiContextServer::start(cfg).unwrap();
        let mut client = Client::connect(&server);
        client.send(HELLO);
        wait_until("wrong uid counted", || server.stats().wrong_uid == 1);
        assert!(client.is_closed_by_peer());
        assert_eq!(server.stats().accepted, 0);
        assert_eq!(
            availability_now(&server),
            ContextAvailability::PermissionNotRequested
        );
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_newer_provider_replaces_the_older_one() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut first = Client::connect(&server).hello();
        let mut second = Client::connect(&server).hello();
        wait_until("replaced", || server.stats().replaced == 1);
        assert!(first.is_closed_by_peer());
        second.send(&context(1, 1, "restricted", "null", "null"));
        wait_until("second applied", || server.stats().applied == 1);
        assert_eq!(availability_now(&server), ContextAvailability::Restricted);
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_flood_is_rate_limited_and_a_broken_provider_is_disconnected() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut client = Client::connect(&server).hello();
        for i in 0..100u64 {
            client.send(&context(i + 1, i, "available", "6", "null"));
        }
        wait_until("flood handled", || server.stats().rate_limited > 0);
        assert!(server.stats().applied <= MAX_MESSAGES_PER_SECOND as u64);
        let mut broken = Client::connect(&server).hello();
        for _ in 0..(MAX_CONSECUTIVE_BAD + 4) {
            broken.send("garbage");
            thread::sleep(Duration::from_millis(60)); // stay under the rate limit
        }
        wait_until("closed", || server.stats().closed_for_bad_lines == 1);
        assert!(broken.is_closed_by_peer());
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn random_bytes_never_stop_the_server_serving_a_good_provider() {
        let dir = temp_dir();
        let server = WifiContextServer::start(config(&dir)).unwrap();
        let mut state = 0x1234_5678_9abc_def0u64;
        for _ in 0..40 {
            let mut noisy = UnixStream::connect(server.socket_path()).unwrap();
            let junk: Vec<u8> = (0..700)
                .map(|_| {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    state as u8
                })
                .collect();
            let _ = noisy.write_all(&junk);
        }
        let mut client = Client::connect(&server).hello();
        client.send(&context(1, 1, "unsupported", "null", "null"));
        wait_until("good provider served", || server.stats().applied >= 1);
        assert_eq!(availability_now(&server), ContextAvailability::Unsupported);
        drop(server);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_stale_socket_is_reclaimed_and_a_live_one_or_a_stranger_is_refused() {
        let dir = temp_dir();
        let first = WifiContextServer::start(config(&dir)).unwrap();
        // A second daemon must not steal the port of a live one.
        let err = WifiContextServer::start(config(&dir)).err().unwrap();
        assert_eq!(err.kind(), io::ErrorKind::AddrInUse);
        // A crashed daemon leaves its socket file behind: bind and drop the
        // listener without removing the file.
        let path = first.socket_path().to_path_buf();
        drop(first);
        drop(UnixListener::bind(&path).unwrap());
        assert!(path.exists());
        let again = WifiContextServer::start(config(&dir)).unwrap();
        drop(again);
        // Something that is not a socket is never deleted.
        fs::write(&path, b"precious").unwrap();
        assert!(WifiContextServer::start(config(&dir)).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"precious");
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_directory_that_is_a_symlink_or_a_too_long_path_is_refused() {
        let dir = temp_dir();
        fs::create_dir_all(&dir).unwrap();
        let target = dir.join("real");
        fs::create_dir(&target).unwrap();
        let link = dir.join("link");
        std::os::unix::fs::symlink(&target, &link).unwrap();
        assert!(WifiContextServer::start(config(&link)).is_err());
        let long = dir.join("x".repeat(120));
        assert!(WifiContextServer::start(config(&long)).is_err());
        fs::remove_dir_all(&dir).unwrap();
    }
}
