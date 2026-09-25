// SPDX-License-Identifier: GPL-3.0-only

//! Single ownership of the USB serial port: an exclusive `flock` on a file in
//! the per-user run directory. The kernel drops the lock when the holder dies,
//! so a crash never leaves a stale lock. Whoever wants to open the port (the
//! daemon, or a foreground CLI run) holds it first.

use std::ffi::c_int;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use platform_macos::ipc::{current_uid, prepare_dir};

pub const LOCK_NAME: &str = "daemon.lock";
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;

extern "C" {
    fn flock(fd: c_int, operation: c_int) -> c_int;
}

#[derive(Debug)]
pub enum LockError {
    /// Another process owns the port; carries its pid when it wrote one.
    Held {
        pid: Option<u32>,
    },
    Io(io::Error),
}

impl std::fmt::Display for LockError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LockError::Held { pid: Some(pid) } => write!(
                f,
                "another cyd-host process (pid {pid}) already owns the display's serial port; \
                 use `cyd-host status` to see it or `cyd-host service stop` to stop it"
            ),
            LockError::Held { pid: None } => write!(
                f,
                "another cyd-host process already owns the display's serial port"
            ),
            LockError::Io(e) => write!(f, "cannot take the single-instance lock: {e}"),
        }
    }
}

impl From<io::Error> for LockError {
    fn from(error: io::Error) -> Self {
        LockError::Io(error)
    }
}

/// Held for as long as this value lives.
#[derive(Debug)]
pub struct InstanceLock {
    _file: File,
}

impl InstanceLock {
    pub fn acquire(run_dir: &Path) -> Result<InstanceLock, LockError> {
        prepare_dir(run_dir, current_uid())?;
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(run_dir.join(LOCK_NAME))?;
        // SAFETY: a valid open descriptor; flock has no memory effects.
        if unsafe { flock(file.as_raw_fd(), LOCK_EX | LOCK_NB) } != 0 {
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::WouldBlock {
                let mut text = String::new();
                let _ = file.read_to_string(&mut text);
                return Err(LockError::Held {
                    pid: text.trim().parse().ok(),
                });
            }
            return Err(error.into());
        }
        file.set_len(0)?;
        file.rewind()?;
        write!(file, "{}", std::process::id())?;
        Ok(InstanceLock { _file: file })
    }

    /// Whether some process holds the lock right now (without taking it).
    pub fn is_held(run_dir: &Path) -> bool {
        matches!(Self::acquire(run_dir), Err(LockError::Held { .. }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("cyd-lock-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn a_second_holder_is_refused_with_the_first_pid_until_the_first_drops() {
        let dir = temp("second");
        let first = InstanceLock::acquire(&dir).unwrap();
        match InstanceLock::acquire(&dir) {
            Err(LockError::Held { pid }) => assert_eq!(pid, Some(std::process::id())),
            other => panic!("expected Held, got {other:?}"),
        }
        assert!(InstanceLock::is_held(&dir));
        drop(first);
        assert!(!InstanceLock::is_held(&dir));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
