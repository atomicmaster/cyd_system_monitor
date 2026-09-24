// SPDX-License-Identifier: GPL-3.0-only

//! The daemon's local status/control IPC (`docs/host-control-ipc.md`).
//!
//! A Unix domain stream socket `control.sock` in the per-user run directory
//! (0700 directory, 0600 socket, peer uid verified with `getpeereid`). One
//! JSON object per line each way, requests at most 512 bytes. Anything
//! malformed, oversized, of an unsupported version or from another uid is
//! dropped without effect and counted. The server knows nothing about the
//! daemon: it parses, verifies and hands a typed [`Request`] to a [`Handler`].

use std::fs::{self};
use std::io::{self, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use metrics::wifi_context::{parse_flat_object, MessageError, Value, MAX_LINE_BYTES};
use platform_macos::wifi_context_server::{clear_stale_socket, peer_uid, prepare_dir};

pub const SOCKET_NAME: &str = "control.sock";
pub const IPC_VERSION: i64 = 1;
/// Longest request line, newline excluded.
pub const MAX_REQUEST_BYTES: usize = MAX_LINE_BYTES;
/// Longest response line the server will send.
pub const MAX_RESPONSE_BYTES: usize = 8192;
const MAX_CONNECTIONS: usize = 8;
const MAX_CONSECUTIVE_BAD: u32 = 16;
/// Each request line must be complete this long after the connection opened
/// or the previous reply was sent; a slower client is closed, so clients
/// trickling bytes cannot hold every connection slot.
const LINE_DEADLINE: Duration = Duration::from_secs(10);
const POLL: Duration = Duration::from_millis(50);
const MAX_SOCKET_PATH: usize = 104;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Request {
    Status,
    Monitors,
    PairStart {
        port: Option<String>,
    },
    PairCancel,
    Unpair,
    ResetCalibration,
    /// Forget the stored display on this Mac only (the display is untouched).
    Forget,
    Stop,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RequestError {
    TooLong,
    Malformed,
    BadVersion,
    UnknownCommand,
}

/// Only device nodes of this shape may be named as the serial port.
fn valid_port(path: &str) -> bool {
    path.strip_prefix("/dev/").is_some_and(|name| {
        !name.is_empty()
            && name.len() <= 64
            && name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
    })
}

pub fn parse_request(line: &[u8]) -> Result<Request, RequestError> {
    let fields = parse_flat_object(line).map_err(|e| match e {
        MessageError::TooLong => RequestError::TooLong,
        _ => RequestError::Malformed,
    })?;
    let get = |key: &str| fields.iter().find(|(k, _)| k == key).map(|(_, v)| v);
    match get("v") {
        Some(Value::Int(IPC_VERSION)) => {}
        Some(Value::Int(_)) => return Err(RequestError::BadVersion),
        _ => return Err(RequestError::Malformed),
    }
    let Some(Value::Str(cmd)) = get("cmd") else {
        return Err(RequestError::Malformed);
    };
    let port = match get("port") {
        None | Some(Value::Null) => None,
        Some(Value::Str(p)) if valid_port(p) => Some(p.clone()),
        Some(_) => return Err(RequestError::Malformed),
    };
    Ok(match cmd.as_str() {
        "status" => Request::Status,
        "monitors" => Request::Monitors,
        "pair_start" => Request::PairStart { port },
        "pair_cancel" => Request::PairCancel,
        "unpair" => Request::Unpair,
        "reset_calibration" => Request::ResetCalibration,
        "forget" => Request::Forget,
        "stop" => Request::Stop,
        _ => return Err(RequestError::UnknownCommand),
    })
}

/// JSON string literal with the escapes JSON requires.
pub fn json_string(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// `{"v":1,"ok":true,"cmd":"<cmd>"[,"<key>":<json>]}`
pub fn ok_response(cmd: &str, body: Option<(&str, &str)>) -> String {
    match body {
        Some((key, json)) => format!(
            "{{\"v\":{IPC_VERSION},\"ok\":true,\"cmd\":{},\"{key}\":{json}}}",
            json_string(cmd)
        ),
        None => format!(
            "{{\"v\":{IPC_VERSION},\"ok\":true,\"cmd\":{}}}",
            json_string(cmd)
        ),
    }
}

/// `{"v":1,"ok":false,"error":"<code>","message":"<text>"}`
pub fn error_response(code: &str, message: &str) -> String {
    format!(
        "{{\"v\":{IPC_VERSION},\"ok\":false,\"error\":{},\"message\":{}}}",
        json_string(code),
        json_string(message)
    )
}

pub trait Handler: Send + Sync {
    /// `stats` are the server's own counters, for the status report.
    fn handle(&self, request: Request, stats: &ControlStats) -> String;
}

/// What the server has refused or dropped since it started.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ControlStats {
    pub accepted: u64,
    pub requests: u64,
    pub wrong_uid: u64,
    pub malformed: u64,
    pub oversized: u64,
    pub bad_version: u64,
    pub unknown_command: u64,
    pub busy: u64,
    pub closed_for_bad_lines: u64,
}

#[derive(Default)]
struct Counters {
    accepted: AtomicU64,
    requests: AtomicU64,
    wrong_uid: AtomicU64,
    malformed: AtomicU64,
    oversized: AtomicU64,
    bad_version: AtomicU64,
    unknown_command: AtomicU64,
    busy: AtomicU64,
    closed_for_bad_lines: AtomicU64,
    active: AtomicUsize,
}

impl Counters {
    fn snapshot(&self) -> ControlStats {
        let get = |a: &AtomicU64| a.load(Ordering::Relaxed);
        ControlStats {
            accepted: get(&self.accepted),
            requests: get(&self.requests),
            wrong_uid: get(&self.wrong_uid),
            malformed: get(&self.malformed),
            oversized: get(&self.oversized),
            bad_version: get(&self.bad_version),
            unknown_command: get(&self.unknown_command),
            busy: get(&self.busy),
            closed_for_bad_lines: get(&self.closed_for_bad_lines),
        }
    }
}

fn bump(counter: &AtomicU64) {
    counter.fetch_add(1, Ordering::Relaxed);
}

pub struct ControlServer {
    path: PathBuf,
    counters: Arc<Counters>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl ControlServer {
    pub fn start(dir: &Path, expected_uid: u32, handler: Arc<dyn Handler>) -> io::Result<Self> {
        Self::start_with_deadline(dir, expected_uid, handler, LINE_DEADLINE)
    }

    /// [`ControlServer::start`] with another per-line deadline (tests).
    pub fn start_with_deadline(
        dir: &Path,
        expected_uid: u32,
        handler: Arc<dyn Handler>,
        line_deadline: Duration,
    ) -> io::Result<Self> {
        let path = dir.join(SOCKET_NAME);
        if path.as_os_str().len() >= MAX_SOCKET_PATH {
            return Err(io::Error::other("the socket path is too long for sun_path"));
        }
        prepare_dir(dir, platform_macos::wifi_context_server::current_uid())?;
        clear_stale_socket(&path, platform_macos::wifi_context_server::current_uid())?;
        let listener = UnixListener::bind(&path)?;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
        listener.set_nonblocking(true)?;
        let counters = Arc::new(Counters::default());
        let stop = Arc::new(AtomicBool::new(false));
        let thread = {
            let (counters, stop) = (counters.clone(), stop.clone());
            thread::Builder::new()
                .name("control-ipc".into())
                .spawn(move || {
                    accept_loop(
                        listener,
                        expected_uid,
                        handler,
                        counters,
                        stop,
                        line_deadline,
                    )
                })?
        };
        Ok(Self {
            path,
            counters,
            stop,
            thread: Some(thread),
        })
    }

    pub fn socket_path(&self) -> &Path {
        &self.path
    }

    pub fn stats(&self) -> ControlStats {
        self.counters.snapshot()
    }
}

impl Drop for ControlServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        let _ = fs::remove_file(&self.path);
    }
}

fn accept_loop(
    listener: UnixListener,
    expected_uid: u32,
    handler: Arc<dyn Handler>,
    counters: Arc<Counters>,
    stop: Arc<AtomicBool>,
    line_deadline: Duration,
) {
    let mut workers: Vec<JoinHandle<()>> = Vec::new();
    while !stop.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, _)) => {
                if peer_uid(&stream) != Some(expected_uid) {
                    bump(&counters.wrong_uid);
                    drop(stream); // closed without a byte read or written
                } else if counters.active.load(Ordering::Relaxed) >= MAX_CONNECTIONS {
                    bump(&counters.busy);
                    drop(stream);
                } else {
                    bump(&counters.accepted);
                    counters.active.fetch_add(1, Ordering::Relaxed);
                    let (handler, counters, stop) =
                        (handler.clone(), counters.clone(), stop.clone());
                    let spawned =
                        thread::Builder::new()
                            .name("control-conn".into())
                            .spawn(move || {
                                serve_connection(
                                    stream,
                                    handler.as_ref(),
                                    &counters,
                                    &stop,
                                    line_deadline,
                                );
                                counters.active.fetch_sub(1, Ordering::Relaxed);
                            });
                    match spawned {
                        Ok(worker) => workers.push(worker),
                        Err(_) => {
                            // Thread exhaustion: the closure never ran.
                        }
                    }
                    workers.retain(|w| !w.is_finished());
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => thread::sleep(POLL),
            Err(_) => thread::sleep(POLL),
        }
    }
    for worker in workers {
        let _ = worker.join();
    }
}

fn serve_connection(
    mut stream: UnixStream,
    handler: &dyn Handler,
    counters: &Counters,
    stop: &AtomicBool,
    line_deadline: Duration,
) {
    if stream.set_read_timeout(Some(POLL)).is_err()
        || stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .is_err()
    {
        return;
    }
    let mut line: Vec<u8> = Vec::new();
    let mut discarding = false;
    let mut consecutive_bad = 0u32;
    // Absolute, not idle: bytes arriving do not extend it, only a whole line.
    let mut line_started = Instant::now();
    let mut buf = [0u8; 512];
    while !stop.load(Ordering::Relaxed) && line_started.elapsed() < line_deadline {
        let n = match stream.read(&mut buf) {
            Ok(0) => return,
            Ok(n) => n,
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) =>
            {
                continue
            }
            Err(_) => return,
        };
        for &byte in &buf[..n] {
            if byte != b'\n' {
                if discarding {
                    continue;
                }
                if line.len() >= MAX_REQUEST_BYTES {
                    // Too long: drop the rest of the line without buffering it.
                    discarding = true;
                    line.clear();
                    continue;
                }
                line.push(byte);
                continue;
            }
            let reply = if discarding {
                discarding = false;
                bump(&counters.oversized);
                consecutive_bad += 1;
                Some(error_response("oversized", "request line too long"))
            } else {
                let parsed = parse_request(&line);
                line.clear();
                match parsed {
                    Ok(request) => {
                        consecutive_bad = 0;
                        bump(&counters.requests);
                        let mut response = handler.handle(request, &counters.snapshot());
                        if response.len() > MAX_RESPONSE_BYTES {
                            response = error_response("response_too_large", "response too large");
                        }
                        Some(response)
                    }
                    Err(RequestError::BadVersion) => {
                        bump(&counters.bad_version);
                        let _ = write_line(
                            &mut stream,
                            &error_response("unsupported_version", "only v1 is supported"),
                        );
                        return;
                    }
                    Err(error) => {
                        consecutive_bad += 1;
                        Some(match error {
                            RequestError::UnknownCommand => {
                                bump(&counters.unknown_command);
                                error_response("unknown_command", "no such command")
                            }
                            RequestError::TooLong => {
                                bump(&counters.oversized);
                                error_response("oversized", "request line too long")
                            }
                            _ => {
                                bump(&counters.malformed);
                                error_response("malformed", "not a valid request")
                            }
                        })
                    }
                }
            };
            if let Some(reply) = reply {
                if write_line(&mut stream, &reply).is_err() {
                    return;
                }
            }
            // The next line's clock starts once this one is answered (a
            // command may take the supervisor many seconds).
            line_started = Instant::now();
            if consecutive_bad >= MAX_CONSECUTIVE_BAD {
                bump(&counters.closed_for_bad_lines);
                return;
            }
        }
    }
}

fn write_line(stream: &mut UnixStream, text: &str) -> io::Result<()> {
    let mut bytes = text.as_bytes().to_vec();
    bytes.push(b'\n');
    stream.write_all(&bytes)
}

/// Sends one request line and reads one response line (bounded to
/// [`MAX_RESPONSE_BYTES`] plus its newline). The CLI's client.
pub fn request(dir: &Path, line: &str, timeout: Duration) -> io::Result<String> {
    let mut stream = UnixStream::connect(dir.join(SOCKET_NAME))?;
    stream.set_read_timeout(Some(timeout))?;
    stream.set_write_timeout(Some(timeout))?;
    write_line(&mut stream, line)?;
    let mut out = Vec::new();
    let mut byte = [0u8; 1];
    loop {
        if stream.read(&mut byte)? == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "the daemon closed the connection without a reply",
            ));
        }
        if byte[0] == b'\n' {
            break;
        }
        out.push(byte[0]);
        if out.len() > MAX_RESPONSE_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "oversized reply",
            ));
        }
    }
    String::from_utf8(out).map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "bad reply"))
}

/// The request line for a command with no arguments.
pub fn simple_request(cmd: &str) -> String {
    format!("{{\"v\":{IPC_VERSION},\"cmd\":{}}}", json_string(cmd))
}

#[cfg(test)]
mod tests {
    use super::*;
    use platform_macos::wifi_context_server::current_uid;
    use std::os::unix::fs::MetadataExt;

    struct Echo;
    impl Handler for Echo {
        fn handle(&self, request: Request, _stats: &ControlStats) -> String {
            match request {
                Request::Status => ok_response("status", Some(("status", "{\"n\":1}"))),
                Request::Stop => ok_response("stop", None),
                other => ok_response(&format!("{other:?}").to_lowercase(), None),
            }
        }
    }

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("cyd-ctl-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    fn start(dir: &Path) -> ControlServer {
        ControlServer::start(dir, current_uid(), Arc::new(Echo)).unwrap()
    }

    fn raw(dir: &Path, bytes: &[u8]) -> String {
        let mut stream = UnixStream::connect(dir.join(SOCKET_NAME)).unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream.write_all(bytes).unwrap();
        let mut out = Vec::new();
        let mut byte = [0u8; 1];
        while stream.read(&mut byte).unwrap_or(0) == 1 {
            out.push(byte[0]);
            if byte[0] == b'\n' {
                break;
            }
        }
        String::from_utf8(out).unwrap()
    }

    #[test]
    fn parses_every_command_and_rejects_the_rest() {
        let line = |cmd: &str| format!("{{\"v\":1,\"cmd\":\"{cmd}\"}}");
        for (cmd, expected) in [
            ("status", Request::Status),
            ("monitors", Request::Monitors),
            ("pair_start", Request::PairStart { port: None }),
            ("pair_cancel", Request::PairCancel),
            ("unpair", Request::Unpair),
            ("reset_calibration", Request::ResetCalibration),
            ("forget", Request::Forget),
            ("stop", Request::Stop),
        ] {
            assert_eq!(parse_request(line(cmd).as_bytes()), Ok(expected), "{cmd}");
        }
        assert_eq!(
            parse_request(br#"{"v":1,"cmd":"pair_start","port":"/dev/cu.usbserial-10"}"#),
            Ok(Request::PairStart {
                port: Some("/dev/cu.usbserial-10".into())
            })
        );
        for bad in [
            &br#"{"v":1,"cmd":"pair_start","port":"/etc/passwd"}"#[..],
            br#"{"v":1,"cmd":"pair_start","port":"/dev/../etc/x"}"#,
            br#"{"cmd":"status"}"#,
            br#"{"v":1}"#,
            br#"{"v":1,"cmd":"status","cmd":"stop"}"#,
            b"not json",
            b"",
        ] {
            assert_eq!(parse_request(bad), Err(RequestError::Malformed), "{bad:?}");
        }
        assert_eq!(
            parse_request(br#"{"v":2,"cmd":"status"}"#),
            Err(RequestError::BadVersion)
        );
        assert_eq!(
            parse_request(br#"{"v":1,"cmd":"reboot"}"#),
            Err(RequestError::UnknownCommand)
        );
        assert_eq!(
            parse_request(&vec![b'x'; MAX_REQUEST_BYTES + 1]),
            Err(RequestError::TooLong)
        );
    }

    #[test]
    fn answers_a_status_request_and_creates_private_paths() {
        let dir = temp("status");
        let server = start(&dir);
        let reply = request(&dir, &simple_request("status"), Duration::from_secs(2)).unwrap();
        assert_eq!(
            reply,
            r#"{"v":1,"ok":true,"cmd":"status","status":{"n":1}}"#
        );
        assert_eq!(fs::metadata(&dir).unwrap().mode() & 0o777, 0o700);
        assert_eq!(
            fs::metadata(server.socket_path()).unwrap().mode() & 0o777,
            0o600
        );
        // A second server on the same directory refuses while the first lives.
        assert!(ControlServer::start(&dir, current_uid(), Arc::new(Echo)).is_err());
        drop(server);
        assert!(!dir.join(SOCKET_NAME).exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn malformed_oversized_and_unversioned_input_is_dropped_and_counted() {
        let dir = temp("bad");
        let server = start(&dir);
        assert!(raw(&dir, b"garbage\n").contains("\"error\":\"malformed\""));
        let mut long = vec![b'a'; 5_000];
        long.push(b'\n');
        assert!(raw(&dir, &long).contains("\"error\":\"oversized\""));
        assert!(raw(&dir, b"{\"v\":9,\"cmd\":\"status\"}\n").contains("unsupported_version"));
        assert!(raw(&dir, b"{\"v\":1,\"cmd\":\"nope\"}\n").contains("unknown_command"));
        // A good request after bad ones on one connection still works.
        let mut stream = UnixStream::connect(dir.join(SOCKET_NAME)).unwrap();
        stream.write_all(b"junk\n").unwrap();
        stream
            .write_all(simple_request("status").as_bytes())
            .unwrap();
        stream.write_all(b"\n").unwrap();
        let mut got = String::new();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let mut buf = [0u8; 512];
        while got.matches('\n').count() < 2 {
            let n = stream.read(&mut buf).unwrap();
            got.push_str(std::str::from_utf8(&buf[..n]).unwrap());
        }
        assert!(got.contains("\"ok\":true"));
        let stats = server.stats();
        assert_eq!(
            (
                stats.malformed,
                stats.oversized,
                stats.bad_version,
                stats.unknown_command,
                stats.requests
            ),
            (2, 1, 1, 1, 1)
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_connection_from_another_uid_is_closed_unread_and_counted() {
        let dir = temp("uid");
        let server =
            ControlServer::start(&dir, current_uid().wrapping_add(1), Arc::new(Echo)).unwrap();
        let mut stream = UnixStream::connect(dir.join(SOCKET_NAME)).unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let _ = stream.write_all(simple_request("stop").as_bytes());
        let mut buf = [0u8; 16];
        assert_eq!(
            stream.read(&mut buf).unwrap_or(0),
            0,
            "no byte for a stranger"
        );
        std::thread::sleep(Duration::from_millis(100));
        let stats = server.stats();
        assert_eq!((stats.wrong_uid, stats.requests), (1, 0));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn clients_trickling_bytes_are_closed_at_the_line_deadline_and_cannot_lock_others_out() {
        let dir = temp("trickle");
        let deadline = Duration::from_millis(400);
        let server =
            ControlServer::start_with_deadline(&dir, current_uid(), Arc::new(Echo), deadline)
                .unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        // Every slot taken by a client sending one byte every 20 ms, never a
        // whole line.
        let tricklers: Vec<_> = (0..MAX_CONNECTIONS)
            .map(|_| {
                let mut stream = UnixStream::connect(dir.join(SOCKET_NAME)).unwrap();
                let stop = stop.clone();
                thread::spawn(move || {
                    let opened = Instant::now();
                    while !stop.load(Ordering::Relaxed) {
                        if stream.write_all(b" ").is_err() {
                            return Some(opened.elapsed());
                        }
                        thread::sleep(Duration::from_millis(20));
                    }
                    None
                })
            })
            .collect();
        let end = Instant::now() + Duration::from_secs(5);
        while server.stats().accepted < MAX_CONNECTIONS as u64 {
            assert!(Instant::now() < end, "tricklers not accepted");
            thread::sleep(Duration::from_millis(10));
        }
        thread::sleep(deadline * 2);
        // The slow clients are gone: the app or CLI is answered.
        let reply = request(&dir, &simple_request("status"), Duration::from_secs(2));
        stop.store(true, Ordering::Relaxed);
        assert!(reply.is_ok_and(|r| r.contains("\"ok\":true")));
        for trickler in tricklers {
            let closed_after = trickler.join().unwrap().expect("closed by the server");
            assert!(closed_after < deadline * 3, "{closed_after:?}");
        }
        drop(server);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_line_on_time_after_a_slow_reply_is_still_served() {
        let dir = temp("ontime");
        let deadline = Duration::from_millis(300);
        let _server =
            ControlServer::start_with_deadline(&dir, current_uid(), Arc::new(Echo), deadline)
                .unwrap();
        let mut stream = UnixStream::connect(dir.join(SOCKET_NAME)).unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let mut buf = [0u8; 256];
        for _ in 0..3 {
            // Each request arrives within the deadline of the previous reply,
            // though the connection as a whole lives longer than it.
            thread::sleep(deadline / 2);
            stream
                .write_all(format!("{}\n", simple_request("status")).as_bytes())
                .unwrap();
            let n = stream.read(&mut buf).unwrap();
            assert!(std::str::from_utf8(&buf[..n])
                .unwrap()
                .contains("\"ok\":true"));
        }
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn sixteen_bad_lines_in_a_row_close_the_connection() {
        let dir = temp("flood");
        let server = start(&dir);
        let mut stream = UnixStream::connect(dir.join(SOCKET_NAME)).unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        for _ in 0..MAX_CONSECUTIVE_BAD {
            let _ = stream.write_all(b"x\n");
        }
        let mut sink = Vec::new();
        let _ = stream.read_to_end(&mut sink);
        assert_eq!(server.stats().closed_for_bad_lines, 1);
        let _ = fs::remove_dir_all(&dir);
    }
}
