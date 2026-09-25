// SPDX-License-Identifier: GPL-3.0-only

//! What the daemon's two local sockets share (the WiFi context provider
//! socket, `docs/wifi-context-provider.md`, and the control socket,
//! `docs/host-control-ipc.md`): a private per-user run directory (0700), a
//! 0600 socket bound in it after a stale one is reclaimed, the peer's uid, a
//! bounded line reader, and a server thread that is stopped, joined and whose
//! socket is removed on drop.

use std::ffi::c_int;
use std::fs::{self, DirBuilder};
use std::io;
use std::os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, PermissionsExt};
use std::os::unix::io::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};

/// `sizeof(sockaddr_un.sun_path)` on Darwin; the path plus NUL must fit.
pub const MAX_SOCKET_PATH: usize = 104;
/// Bad lines in a row before a connection is dropped as a broken peer.
pub const MAX_CONSECUTIVE_BAD: u32 = 16;

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

/// Binds `dir/name` as a private, non-blocking listening socket: the path
/// must fit `sun_path`, the directory is prepared, a stale socket reclaimed,
/// and the socket file is 0600 (the 0700 directory already keeps everyone
/// else out; this is the documented file mode as well).
pub fn bind_private(dir: &Path, name: &str) -> io::Result<(PathBuf, UnixListener)> {
    let path = dir.join(name);
    if path.as_os_str().len() >= MAX_SOCKET_PATH {
        return Err(io::Error::other("the socket path is too long for sun_path"));
    }
    prepare_dir(dir, current_uid())?;
    clear_stale_socket(&path, current_uid())?;
    let listener = UnixListener::bind(&path)?;
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
    listener.set_nonblocking(true)?;
    Ok((path, listener))
}

/// A socket server's thread. Dropping it asks the thread to stop, waits for
/// it, and removes the socket file.
pub struct ServerThread {
    path: PathBuf,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl ServerThread {
    /// Binds `dir/name` ([`bind_private`]) and runs `serve` on a thread named
    /// `thread_name` until the stop flag it is given is set.
    pub fn spawn(
        dir: &Path,
        name: &str,
        thread_name: &str,
        serve: impl FnOnce(UnixListener, Arc<AtomicBool>) + Send + 'static,
    ) -> io::Result<Self> {
        let (path, listener) = bind_private(dir, name)?;
        let stop = Arc::new(AtomicBool::new(false));
        let thread = {
            let stop = stop.clone();
            thread::Builder::new()
                .name(thread_name.into())
                .spawn(move || serve(listener, stop))?
        };
        Ok(Self {
            path,
            stop,
            thread: Some(thread),
        })
    }

    pub fn socket_path(&self) -> &Path {
        &self.path
    }
}

impl Drop for ServerThread {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        let _ = fs::remove_file(&self.path);
    }
}

/// What one byte did to a [`LineReader`].
#[derive(Debug, PartialEq, Eq)]
pub enum LineEvent {
    /// Nothing complete yet.
    Pending,
    /// A whole line (newline excluded; possibly empty).
    Line(Vec<u8>),
    /// The line just exceeded the bound: it is dropped, and its remaining
    /// bytes are discarded unbuffered up to its newline.
    Overflowed,
    /// The newline that ends a line that overflowed.
    OversizedEnd,
}

/// Splits a byte stream into newline-terminated lines of at most `max`
/// bytes, never buffering more than that.
#[derive(Debug)]
pub struct LineReader {
    max: usize,
    line: Vec<u8>,
    discarding: bool,
}

impl LineReader {
    pub fn new(max: usize) -> Self {
        Self {
            max,
            line: Vec::new(),
            discarding: false,
        }
    }

    pub fn push(&mut self, byte: u8) -> LineEvent {
        if byte == b'\n' {
            if self.discarding {
                self.discarding = false;
                return LineEvent::OversizedEnd;
            }
            return LineEvent::Line(std::mem::take(&mut self.line));
        }
        if self.discarding {
            return LineEvent::Pending;
        }
        if self.line.len() >= self.max {
            self.discarding = true;
            self.line.clear();
            return LineEvent::Overflowed;
        }
        self.line.push(byte);
        LineEvent::Pending
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn feed(reader: &mut LineReader, bytes: &[u8]) -> Vec<LineEvent> {
        bytes
            .iter()
            .map(|&b| reader.push(b))
            .filter(|e| *e != LineEvent::Pending)
            .collect()
    }

    #[test]
    fn lines_are_split_and_an_oversized_one_is_dropped_unbuffered() {
        let mut reader = LineReader::new(3);
        assert_eq!(
            feed(&mut reader, b"abc\n\nabcdef\nxy\n"),
            vec![
                LineEvent::Line(b"abc".to_vec()),
                LineEvent::Line(Vec::new()),
                LineEvent::Overflowed,
                LineEvent::OversizedEnd,
                LineEvent::Line(b"xy".to_vec()),
            ]
        );
        assert!(reader.line.capacity() <= 8);
    }
}
