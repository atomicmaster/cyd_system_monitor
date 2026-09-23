// SPDX-License-Identifier: GPL-3.0-only

//! The daemon's routine log. Policy (docs/macos-components.md): rotate after
//! seven days or 50 MiB, whichever comes first. The current file is
//! `hostd.log`; on rotation it becomes `hostd.log.1`, replacing any older
//! rotation, so at most two files (about 100 MiB in the worst case) exist.
//! Only state changes are logged, never per-second metrics, radio payloads,
//! prompts or personal data.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, SystemTime};

pub const MAX_BYTES: u64 = 50 * 1024 * 1024;
pub const MAX_AGE: Duration = Duration::from_secs(7 * 24 * 60 * 60);
const FILE: &str = "hostd.log";

pub struct RotatingLog {
    dir: PathBuf,
    file: File,
    size: u64,
    started: SystemTime,
    max_bytes: u64,
    max_age: Duration,
}

impl RotatingLog {
    pub fn open(dir: &Path) -> io::Result<Self> {
        Self::open_with(dir, MAX_BYTES, MAX_AGE)
    }

    pub fn open_with(dir: &Path, max_bytes: u64, max_age: Duration) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        let path = dir.join(FILE);
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .mode(0o600)
            .open(&path)?;
        let meta = file.metadata()?;
        let started = meta.created().or_else(|_| meta.modified())?;
        let mut log = Self {
            dir: dir.into(),
            size: meta.len(),
            file,
            started,
            max_bytes,
            max_age,
        };
        log.rotate_if_due()?;
        Ok(log)
    }

    fn rotate_if_due(&mut self) -> io::Result<()> {
        let old = SystemTime::now()
            .duration_since(self.started)
            .is_ok_and(|age| age >= self.max_age);
        if self.size < self.max_bytes && !old {
            return Ok(());
        }
        fs::rename(self.dir.join(FILE), self.dir.join(format!("{FILE}.1")))?;
        self.file = OpenOptions::new()
            .create(true)
            .append(true)
            .mode(0o600)
            .open(self.dir.join(FILE))?;
        self.size = 0;
        self.started = SystemTime::now();
        Ok(())
    }

    pub fn write_line(&mut self, line: &str) -> io::Result<()> {
        self.rotate_if_due()?;
        let text = format!("{line}\n");
        self.file.write_all(text.as_bytes())?;
        self.size += text.len() as u64;
        Ok(())
    }
}

/// Where the daemon's lines go.
pub enum Logger {
    Stderr,
    File(Mutex<RotatingLog>),
    /// Discards everything (tests).
    Null,
}

impl Logger {
    pub fn line(&self, text: &str) {
        let stamped = format!("{} {text}", crate::link::utc_ms());
        match self {
            Logger::Stderr => eprintln!("{stamped}"),
            Logger::File(log) => {
                let _ = log
                    .lock()
                    .unwrap_or_else(|p| p.into_inner())
                    .write_line(&stamped);
            }
            Logger::Null => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("cyd-log-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn rotates_at_the_size_limit_keeping_one_previous_file() {
        let dir = temp("size");
        let mut log = RotatingLog::open_with(&dir, 64, MAX_AGE).unwrap();
        for i in 0..20 {
            log.write_line(&format!("line number {i}")).unwrap();
        }
        assert!(dir.join("hostd.log.1").exists());
        assert!(fs::metadata(dir.join("hostd.log")).unwrap().len() <= 64 + 20);
        assert!(!dir.join("hostd.log.2").exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn rotates_when_the_file_is_older_than_the_age_limit() {
        let dir = temp("age");
        {
            let mut log = RotatingLog::open_with(&dir, MAX_BYTES, MAX_AGE).unwrap();
            log.write_line("first").unwrap();
        }
        std::thread::sleep(Duration::from_millis(150));
        let mut log = RotatingLog::open_with(&dir, MAX_BYTES, Duration::from_millis(100)).unwrap();
        log.write_line("second").unwrap();
        assert!(fs::read_to_string(dir.join("hostd.log.1"))
            .unwrap()
            .contains("first"));
        let _ = fs::remove_dir_all(&dir);
    }
}
