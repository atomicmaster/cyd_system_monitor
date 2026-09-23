// SPDX-License-Identifier: GPL-3.0-only

//! Agent Activity: same-user process presence for Codex, Claude and Grok Build
//! (5 s cadence, its own age).
//!
//! The process list comes from `libproc`: `proc_listallpids` for the pids,
//! `proc_pidinfo(PROC_PIDTBSDINFO)` for the owner uid only, and `proc_pidpath`
//! for the executable path, which is then resolved with `realpath`. Only
//! processes of the current uid are looked at, and only their resolved path is
//! kept: no command-line arguments, environment, working directory, open files
//! or process names are read or stored. Classification is the pure
//! `metrics::agents` allowlist. No root, no new dependency.
//!
//! The scan is bounded (at most [`MAX_PIDS`] pids, a wall-clock budget of
//! [`SCAN_BUDGET_MS`]) and runs on the `collect` call only when the cached
//! sample is at least [`REFRESH_MS`] old, so the one-second loop is not
//! blocked (measured cost on the development Mac: see `docs/agent-integrations.md`).
//! A process that exits between listing and inspection is skipped; it does not
//! make the whole source an error. A budget overrun or a failed listing is a
//! failed scan: the last good sample is retained (its age keeps growing, and
//! the device marks it stale after 15 s) until [`RETAIN_MS`], then the source
//! is reported as an error.

use metrics::agents::{classify, AgentState, ProcessRecord, AGENT_COUNT};
use metrics::{Collector, MetricAvailability, MetricSample};

/// How long a scan is reused before processes are listed again.
pub const REFRESH_MS: u64 = 5_000;
/// A failed scan keeps the last sample this long (three refreshes) before the
/// source is reported as an error.
pub const RETAIN_MS: u64 = 15_000;
/// At most this many pids are examined per scan; a larger table fails the scan
/// rather than silently reporting "not running" for the unexamined ones.
pub const MAX_PIDS: usize = 4_096;
/// Wall-clock limit for one scan.
pub const SCAN_BUDGET_MS: u64 = 100;

/// One scan's result: the same-user processes' resolved executable paths, or
/// `None` when the process list could not be obtained (or the scan overran).
pub type ProcessSource = Box<dyn FnMut() -> Option<Vec<ProcessRecord>>>;

/// What the collector reports: the per-agent states and the age of the scan
/// they come from. States are only ever not-running or activity-unknown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AgentsReading {
    pub states: [AgentState; AGENT_COUNT],
    pub age_ms: u32,
}

pub struct AgentsCollector {
    source: ProcessSource,
    uid: u32,
    home: String,
    sample: Option<([AgentState; AGENT_COUNT], u64)>,
    next_scan_ms: u64,
    last_scan_failed: bool,
}

impl AgentsCollector {
    /// The real libproc-backed collector for the current user.
    pub fn new() -> Self {
        let uid = imp::current_uid();
        let home = imp::resolved_home().unwrap_or_default();
        Self::with_source(Box::new(move || imp::scan(uid)), uid, home)
    }

    /// A collector over any source; `uid` and `home` (the current user's
    /// resolved home directory) are handed to the classifier.
    pub fn with_source(source: ProcessSource, uid: u32, home: String) -> Self {
        Self {
            source,
            uid,
            home,
            sample: None,
            next_scan_ms: 0,
            last_scan_failed: false,
        }
    }
}

impl Default for AgentsCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl Collector for AgentsCollector {
    type Reading = AgentsReading;

    fn collect(&mut self, now_ms: u64) -> MetricSample<AgentsReading> {
        if self.sample.is_none() || now_ms >= self.next_scan_ms {
            match (self.source)() {
                Some(records) => {
                    self.sample = Some((classify(&records, self.uid, &self.home), now_ms));
                    self.next_scan_ms = now_ms.saturating_add(REFRESH_MS);
                    self.last_scan_failed = false;
                }
                // A failed scan is retried on the next call.
                None => {
                    self.next_scan_ms = 0;
                    self.last_scan_failed = true;
                }
            }
        }
        match self.sample {
            Some((states, sampled_ms)) => {
                let age = now_ms.saturating_sub(sampled_ms);
                if age >= RETAIN_MS && self.last_scan_failed {
                    // Scans have kept failing: stop showing an old presence.
                    MetricSample::unavailable(MetricAvailability::Error)
                } else {
                    MetricSample::available(AgentsReading {
                        states,
                        age_ms: age.min(u32::MAX.into()) as u32,
                    })
                }
            }
            None => MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable),
        }
    }
}

/// One real scan, for diagnostics: its states and how long it took.
pub fn probe() -> Option<([AgentState; AGENT_COUNT], std::time::Duration, usize)> {
    let uid = imp::current_uid();
    let home = imp::resolved_home().unwrap_or_default();
    let started = std::time::Instant::now();
    let records = imp::scan(uid)?;
    let elapsed = started.elapsed();
    Some((classify(&records, uid, &home), elapsed, records.len()))
}

#[cfg(target_os = "macos")]
mod imp {
    use super::{MAX_PIDS, SCAN_BUDGET_MS};
    use metrics::agents::{is_resolved_path, ProcessRecord, MAX_PATH_BYTES};
    use std::os::raw::{c_int, c_void};
    use std::time::{Duration, Instant};

    const PROC_PIDTBSDINFO: c_int = 3;
    const PROC_PIDPATHINFO_MAXSIZE: usize = 4 * 1024;

    /// `struct proc_bsdinfo` from `<sys/proc_info.h>`; only `pbi_uid` is read.
    /// The name fields are part of the kernel's fixed layout and are ignored.
    #[repr(C)]
    struct BsdInfo {
        flags: u32,
        status: u32,
        xstatus: u32,
        pid: u32,
        ppid: u32,
        uid: u32,
        gid: u32,
        ruid: u32,
        rgid: u32,
        svuid: u32,
        svgid: u32,
        rfu_1: u32,
        comm: [u8; 16],
        name: [u8; 32],
        nfiles: u32,
        pgid: u32,
        pjobc: u32,
        e_tdev: u32,
        e_tpgid: u32,
        nice: i32,
        start_tvsec: u64,
        start_tvusec: u64,
    }

    extern "C" {
        fn getuid() -> u32;
        fn proc_listallpids(buffer: *mut c_void, buffersize: c_int) -> c_int;
        fn proc_pidinfo(
            pid: c_int,
            flavor: c_int,
            arg: u64,
            buffer: *mut c_void,
            buffersize: c_int,
        ) -> c_int;
        fn proc_pidpath(pid: c_int, buffer: *mut c_void, buffersize: u32) -> c_int;
    }

    pub fn current_uid() -> u32 {
        // SAFETY: getuid has no preconditions and cannot fail.
        unsafe { getuid() }
    }

    /// `$HOME` with symlinks resolved, or `None` if unset or not resolvable.
    pub fn resolved_home() -> Option<String> {
        let home = std::env::var_os("HOME")?;
        let resolved = std::fs::canonicalize(home).ok()?;
        resolved.to_str().map(str::to_owned)
    }

    /// Owner uid of `pid`, or `None` if it is gone or unreadable.
    fn owner(pid: c_int) -> Option<u32> {
        let mut info = std::mem::MaybeUninit::<BsdInfo>::zeroed();
        let size = std::mem::size_of::<BsdInfo>() as c_int;
        // SAFETY: the buffer is a properly sized, writable BsdInfo.
        let got = unsafe { proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, info.as_mut_ptr().cast(), size) };
        if got != size {
            return None;
        }
        // SAFETY: the kernel filled the whole struct (got == size).
        Some(unsafe { info.assume_init() }.uid)
    }

    /// Resolved executable path of `pid`, or `None` if the process exited, the
    /// path is not readable, or it is not a sane absolute path.
    fn resolved_path(pid: c_int) -> Option<String> {
        let mut buf = vec![0u8; PROC_PIDPATHINFO_MAXSIZE];
        // SAFETY: the buffer is writable for PROC_PIDPATHINFO_MAXSIZE bytes.
        let len = unsafe { proc_pidpath(pid, buf.as_mut_ptr().cast(), buf.len() as u32) };
        if len <= 0 || len as usize > MAX_PATH_BYTES {
            return None;
        }
        let raw = std::str::from_utf8(&buf[..len as usize]).ok()?;
        if !is_resolved_path(raw) {
            return None;
        }
        // The exit race applies here too: a path that no longer resolves is skipped.
        let resolved = std::fs::canonicalize(raw).ok()?;
        resolved.to_str().map(str::to_owned)
    }

    /// Lists processes of `uid`. `None` if the listing failed, the pid table is
    /// larger than `MAX_PIDS`, or the wall-clock budget ran out.
    pub fn scan(uid: u32) -> Option<Vec<ProcessRecord>> {
        let started = Instant::now();
        let budget = Duration::from_millis(SCAN_BUDGET_MS);
        let mut pids = vec![0 as c_int; MAX_PIDS];
        // SAFETY: the buffer holds MAX_PIDS c_ints and the size says so.
        let count = unsafe {
            proc_listallpids(
                pids.as_mut_ptr().cast(),
                (pids.len() * std::mem::size_of::<c_int>()) as c_int,
            )
        };
        if count <= 0 || count as usize >= MAX_PIDS {
            return None;
        }
        let mut records = Vec::new();
        for &pid in &pids[..count as usize] {
            if pid <= 0 {
                continue;
            }
            if started.elapsed() > budget {
                return None;
            }
            // A process that exited since the listing is skipped, not an error.
            if owner(pid) != Some(uid) {
                continue;
            }
            if let Some(path) = resolved_path(pid) {
                records.push(ProcessRecord { uid, path });
            }
        }
        Some(records)
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    use metrics::agents::ProcessRecord;

    pub fn current_uid() -> u32 {
        0
    }
    pub fn resolved_home() -> Option<String> {
        None
    }
    pub fn scan(_uid: u32) -> Option<Vec<ProcessRecord>> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;

    const UID: u32 = 501;
    const CLAUDE: &str = "/opt/homebrew/Caskroom/claude-code@latest/2.1.280/claude";

    fn rec(path: &str) -> ProcessRecord {
        ProcessRecord {
            uid: UID,
            path: path.to_string(),
        }
    }

    type Script = Rc<RefCell<Vec<Option<Vec<ProcessRecord>>>>>;

    fn collector(script: Script, calls: Rc<Cell<u32>>) -> AgentsCollector {
        AgentsCollector::with_source(
            Box::new(move || {
                calls.set(calls.get() + 1);
                let mut script = script.borrow_mut();
                if script.len() > 1 {
                    script.remove(0)
                } else {
                    script[0].clone()
                }
            }),
            UID,
            "/Users/tester".to_string(),
        )
    }

    #[test]
    fn a_scan_is_reused_for_five_seconds_and_ages() {
        let calls = Rc::new(Cell::new(0));
        let script = Rc::new(RefCell::new(vec![Some(vec![rec(CLAUDE)])]));
        let mut c = collector(script, calls.clone());
        let first = c.collect(1_000).value.unwrap();
        assert_eq!(first.states[1], AgentState::RunningActivityUnknown);
        assert_eq!((first.states[0], first.age_ms), (AgentState::NotRunning, 0));
        let later = c.collect(5_999).value.unwrap();
        assert_eq!((calls.get(), later.age_ms), (1, 4_999));
        let refreshed = c.collect(6_000).value.unwrap();
        assert_eq!((calls.get(), refreshed.age_ms), (2, 0));
    }

    #[test]
    fn zero_matching_processes_is_available_and_not_running() {
        let script = Rc::new(RefCell::new(vec![Some(vec![rec("/usr/bin/zsh")])]));
        let mut c = collector(script, Rc::new(Cell::new(0)));
        let sample = c.collect(0);
        assert_eq!(sample.availability, MetricAvailability::Available);
        assert_eq!(sample.value.unwrap().states, [AgentState::NotRunning; 3]);
        let script = Rc::new(RefCell::new(vec![Some(vec![])]));
        let mut c = collector(script, Rc::new(Cell::new(0)));
        assert_eq!(c.collect(0).availability, MetricAvailability::Available);
    }

    #[test]
    fn a_listing_failure_is_unavailable_never_not_running() {
        let script = Rc::new(RefCell::new(vec![None]));
        let mut c = collector(script, Rc::new(Cell::new(0)));
        let sample = c.collect(0);
        assert_eq!(
            sample.availability,
            MetricAvailability::TemporarilyUnavailable
        );
        assert!(sample.value.is_none());
    }

    #[test]
    fn a_failed_refresh_retains_the_old_sample_then_reports_an_error() {
        let calls = Rc::new(Cell::new(0));
        let script = Rc::new(RefCell::new(vec![Some(vec![rec(CLAUDE)]), None]));
        let mut c = collector(script, calls.clone());
        c.collect(1_000);
        // Refresh fails: the sample stays, with its growing age, and the scan
        // is retried on every call.
        let kept = c.collect(6_000).value.unwrap();
        assert_eq!(kept.states[1], AgentState::RunningActivityUnknown);
        assert_eq!(kept.age_ms, 5_000);
        let kept = c.collect(7_000).value.unwrap();
        assert_eq!((kept.age_ms, calls.get()), (6_000, 3));
        // Retention lapses: an error, not a stale "running" forever.
        let gone = c.collect(16_000);
        assert_eq!(gone.availability, MetricAvailability::Error);
    }

    #[test]
    fn a_recovered_scan_replaces_the_error() {
        let script = Rc::new(RefCell::new(vec![
            Some(vec![rec(CLAUDE)]),
            None,
            None,
            None,
            Some(vec![]),
        ]));
        let mut c = collector(script, Rc::new(Cell::new(0)));
        c.collect(0);
        c.collect(5_000);
        c.collect(10_000);
        assert_eq!(c.collect(15_000).availability, MetricAvailability::Error);
        let back = c.collect(16_000).value.unwrap();
        assert_eq!((back.states, back.age_ms), ([AgentState::NotRunning; 3], 0));
    }

    #[test]
    fn a_scan_on_this_machine_finds_only_states_and_finishes_quickly() {
        if !cfg!(target_os = "macos") {
            return;
        }
        let (states, elapsed, same_user) = probe().expect("libproc lists processes");
        // Only states and counts are printed, never a path (`--nocapture`).
        eprintln!("agents scan: states {states:?}, {same_user} same-user processes, {elapsed:?}");
        assert!(same_user > 0, "this process itself is a same-user process");
        assert!(states.len() == AGENT_COUNT);
        assert!(
            elapsed < std::time::Duration::from_millis(SCAN_BUDGET_MS),
            "{elapsed:?}"
        );
    }
}
