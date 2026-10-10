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
//! `metrics::agents` allowlist. For allowlisted processes only, the cumulative
//! CPU time is read (`proc_pidinfo(PROC_PIDTASKINFO)`, user + system) for the
//! busy/idle heuristic; the baseline is the previous scan, dropped on wake.
//! Claude's activity instead comes from its hooks: `cyd-host agent-event`
//! ([`record_hook_event`]) writes `busy`/`idle` to `<events dir>/claude-<pid>`
//! for the Claude process that ran the hook, and each scan reads those files
//! ([`read_hook_events`]). No root, no new dependency.
//!
//! The scan is bounded (at most [`MAX_PIDS`] pids, a wall-clock budget of
//! [`SCAN_BUDGET_MS`]) and runs on the `collect` call only when the cached
//! sample is at least [`REFRESH_MS`] old, so the one-second loop is not
//! blocked (measured cost on the development Mac: see `docs/agent-integrations.md`).
//! A process that exits between listing and inspection is skipped; it does not
//! make the whole source an error. A budget overrun or a failed listing is a
//! failed scan: the last good sample is retained (its age keeps growing, and
//! the device marks it stale after 15 s) until [`RETAIN_MS`], then the source
//! is reported as an error. With no good sample yet, the source is temporarily
//! unavailable until scans have failed for [`RETAIN_MS`], then an error. A
//! failed scan is retried at the normal [`REFRESH_MS`] cadence.

use metrics::agents::{
    classify, classify_activity, hook_file_name, parse_hook_file, AgentState, CpuBaseline,
    HookActivity, HookRecord, ProcessRecord, AGENT_COUNT,
};
use metrics::cache::{RefreshPolicy, SampleCache};
use metrics::{Collector, MetricAvailability, MetricSample};
use std::path::{Path, PathBuf};

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
/// The Claude hook event files present now (empty when there are none).
pub type HookSource = Box<dyn FnMut() -> Vec<HookRecord>>;

/// At most this many event files are read per scan, each at most this long.
pub const MAX_HOOK_FILES: usize = 64;
const MAX_HOOK_FILE_BYTES: u64 = 16;

/// The event files in `dir` (`claude-<pid>` holding `busy` or `idle`), with
/// their age from the modification time. Unreadable or malformed files and a
/// missing directory yield nothing.
pub fn read_hook_events(dir: &Path) -> Vec<HookRecord> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let now = std::time::SystemTime::now();
    let mut hooks = Vec::new();
    for entry in entries.flatten().take(MAX_HOOK_FILES) {
        let Ok(meta) = entry.metadata() else { continue };
        if !meta.is_file() || meta.len() > MAX_HOOK_FILE_BYTES {
            continue;
        }
        let name = entry.file_name();
        let (Some(name), Ok(contents)) = (name.to_str(), std::fs::read_to_string(entry.path()))
        else {
            continue;
        };
        let Some((pid, activity)) = parse_hook_file(name, &contents) else {
            continue;
        };
        let age_ms = meta
            .modified()
            .ok()
            .and_then(|m| now.duration_since(m).ok())
            .map_or(0, |d| d.as_millis().min(u64::MAX as u128) as u64);
        hooks.push(HookRecord {
            pid,
            activity,
            age_ms,
        });
    }
    hooks
}

/// Records a hook event for the Claude process `pid`: writes `busy`/`idle` to
/// `<dir>/claude-<pid>` atomically (temp file, then rename) in a `0700`
/// directory, and removes files of Claude processes that no longer exist.
pub fn record_hook_event(dir: &Path, pid: i32, activity: HookActivity) -> std::io::Result<()> {
    use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
    std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(dir)?;
    std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700))?;
    let target = dir.join(hook_file_name(pid));
    let temp = dir.join(format!(".{}.tmp", hook_file_name(pid)));
    let text = match activity {
        HookActivity::Busy => "busy\n",
        HookActivity::Idle => "idle\n",
    };
    std::fs::write(&temp, text)?;
    std::fs::rename(&temp, &target)?;
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten().take(MAX_HOOK_FILES) {
            let name = entry.file_name();
            let Some((other, _)) = name.to_str().and_then(|n| parse_hook_file(n, "idle")) else {
                continue;
            };
            if other != pid && !imp::process_exists(other) {
                let _ = std::fs::remove_file(entry.path());
            }
        }
    }
    Ok(())
}

/// The nearest ancestor of this process (at most eight levels up) that is a
/// running Claude agent of the current user: the Claude process whose hook is
/// running this program.
pub fn hook_caller_claude_pid() -> Option<i32> {
    let uid = imp::current_uid();
    let home = imp::resolved_home().unwrap_or_default();
    imp::claude_ancestor(uid, &home)
}

/// What the collector reports: the per-agent states and the age of the scan
/// they come from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AgentsReading {
    pub states: [AgentState; AGENT_COUNT],
    pub age_ms: u32,
}

pub struct AgentsCollector {
    source: ProcessSource,
    uid: u32,
    home: String,
    presence: SampleCache<[AgentState; AGENT_COUNT]>,
    cpu: Option<CpuBaseline>,
    hooks: HookSource,
}

impl AgentsCollector {
    /// The real libproc-backed collector for the current user, reading Claude
    /// hook events from `events_dir`.
    pub fn new(events_dir: PathBuf) -> Self {
        let uid = imp::current_uid();
        let home = imp::resolved_home().unwrap_or_default();
        let scan_home = home.clone();
        let mut collector =
            Self::with_source(Box::new(move || imp::scan(uid, &scan_home)), uid, home);
        collector.hooks = Box::new(move || read_hook_events(&events_dir));
        collector
    }

    /// Replaces the hook event source (tests).
    pub fn with_hooks(mut self, hooks: HookSource) -> Self {
        self.hooks = hooks;
        self
    }

    /// A collector over any source; `uid` and `home` (the current user's
    /// resolved home directory) are handed to the classifier.
    pub fn with_source(source: ProcessSource, uid: u32, home: String) -> Self {
        Self {
            source,
            uid,
            home,
            // Success or failure, the next scan is one refresh away: a failing
            // listing is not retried on every one-second tick.
            presence: SampleCache::new(RefreshPolicy {
                refresh_ms: REFRESH_MS,
                retry_until_sampled: false,
            }),
            cpu: None,
            hooks: Box::new(Vec::new),
        }
    }
}

impl Collector for AgentsCollector {
    type Reading = AgentsReading;

    fn collect(&mut self, now_ms: u64) -> MetricSample<AgentsReading> {
        let (source, uid, home, cpu, hooks) = (
            &mut self.source,
            self.uid,
            &self.home,
            &mut self.cpu,
            &mut self.hooks,
        );
        self.presence.refresh_with(now_ms, || {
            source().map(|records| {
                let events = hooks();
                let (states, next) =
                    classify_activity(&records, uid, home, cpu.as_ref(), now_ms, &events);
                *cpu = Some(next);
                states
            })
        });
        let failing_since = self.presence.failing_since_ms();
        match self.presence.get(now_ms) {
            Some(cached) => {
                if cached.age_ms >= RETAIN_MS && failing_since.is_some() {
                    // Scans have kept failing: stop showing an old presence.
                    MetricSample::unavailable(MetricAvailability::Error)
                } else {
                    MetricSample::available(AgentsReading {
                        states: *cached.value,
                        age_ms: cached.wire_age_ms(),
                    })
                }
            }
            // Never listed: a transient failure first, an error once scans
            // have failed as long as a retained sample would have lasted.
            None => MetricSample::unavailable(match failing_since {
                Some(since) if now_ms.saturating_sub(since) >= RETAIN_MS => {
                    MetricAvailability::Error
                }
                _ => MetricAvailability::TemporarilyUnavailable,
            }),
        }
    }

    /// Scan again before the next snapshot; a failing rescan leaves the old
    /// presence to age out by its sleep-inclusive age.
    fn on_wake(&mut self) {
        self.presence.expire();
        // CPU time does not advance in sleep: never compare across one.
        self.cpu = None;
    }
}

/// One real scan, for diagnostics: its states and how long it took.
pub fn probe() -> Option<([AgentState; AGENT_COUNT], std::time::Duration, usize)> {
    let uid = imp::current_uid();
    let home = imp::resolved_home().unwrap_or_default();
    let started = std::time::Instant::now();
    let records = imp::scan(uid, &home)?;
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
    const PROC_PIDTASKINFO: c_int = 4;

    /// `struct proc_taskinfo` from `<sys/proc_info.h>`; only the two CPU totals
    /// (Mach absolute time units) are read.
    #[repr(C)]
    struct TaskInfo {
        virtual_size: u64,
        resident_size: u64,
        total_user: u64,
        total_system: u64,
        threads_user: u64,
        threads_system: u64,
        counters: [i32; 12],
    }

    #[repr(C)]
    struct Timebase {
        numer: u32,
        denom: u32,
    }
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
        fn mach_timebase_info(info: *mut Timebase) -> c_int;
    }

    /// Cumulative CPU time (user + system) of `pid` in ns, or `None`.
    fn cpu_ns(pid: c_int) -> Option<u64> {
        let mut info = std::mem::MaybeUninit::<TaskInfo>::zeroed();
        let size = std::mem::size_of::<TaskInfo>() as c_int;
        // SAFETY: the buffer is a properly sized, writable TaskInfo.
        let got = unsafe { proc_pidinfo(pid, PROC_PIDTASKINFO, 0, info.as_mut_ptr().cast(), size) };
        if got != size {
            return None;
        }
        // SAFETY: the kernel filled the whole struct (got == size).
        let info = unsafe { info.assume_init() };
        let mut timebase = Timebase { numer: 0, denom: 0 };
        // SAFETY: a valid out-pointer; the call cannot fail on macOS.
        unsafe { mach_timebase_info(&mut timebase) };
        if timebase.denom == 0 {
            return None;
        }
        let ticks = info.total_user.saturating_add(info.total_system) as u128;
        u64::try_from(ticks * timebase.numer as u128 / timebase.denom as u128).ok()
    }

    pub fn current_uid() -> u32 {
        // SAFETY: getuid has no preconditions and cannot fail.
        unsafe { getuid() }
    }

    extern "C" {
        fn getppid() -> c_int;
        fn kill(pid: c_int, sig: c_int) -> c_int;
    }

    /// Whether `pid` exists (signal 0 only checks; EPERM still means it exists).
    pub fn process_exists(pid: i32) -> bool {
        // SAFETY: signal 0 sends nothing.
        let sent = unsafe { kill(pid, 0) };
        sent == 0 || std::io::Error::last_os_error().raw_os_error() == Some(1)
    }

    /// `(owner uid, parent pid)` of `pid`.
    fn owner_and_parent(pid: c_int) -> Option<(u32, c_int)> {
        let mut info = std::mem::MaybeUninit::<BsdInfo>::zeroed();
        let size = std::mem::size_of::<BsdInfo>() as c_int;
        // SAFETY: the buffer is a properly sized, writable BsdInfo.
        let got = unsafe { proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, info.as_mut_ptr().cast(), size) };
        if got != size {
            return None;
        }
        // SAFETY: the kernel filled the whole struct (got == size).
        let info = unsafe { info.assume_init() };
        Some((info.uid, info.ppid as c_int))
    }

    /// Walks up from this process's parent, at most eight levels, to the first
    /// process of `uid` whose resolved path is the Claude agent.
    pub fn claude_ancestor(uid: u32, home: &str) -> Option<i32> {
        // SAFETY: getppid has no preconditions.
        let mut pid = unsafe { getppid() };
        for _ in 0..8 {
            if pid <= 1 {
                return None;
            }
            let (owner, parent) = owner_and_parent(pid)?;
            if owner == uid
                && resolved_path(pid).is_some_and(|path| {
                    metrics::agents::identify(&path, home) == Some(metrics::agents::Agent::Claude)
                })
            {
                return Some(pid);
            }
            pid = parent;
        }
        None
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
    pub fn scan(uid: u32, home: &str) -> Option<Vec<ProcessRecord>> {
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
                // CPU time only for allowlisted processes.
                let cpu_ns = metrics::agents::identify(&path, home).and_then(|_| cpu_ns(pid));
                records.push(ProcessRecord {
                    uid,
                    pid,
                    path,
                    cpu_ns,
                });
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
    pub fn scan(_uid: u32, _home: &str) -> Option<Vec<ProcessRecord>> {
        None
    }
    pub fn process_exists(_pid: i32) -> bool {
        false
    }
    pub fn claude_ancestor(_uid: u32, _home: &str) -> Option<i32> {
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
    const CODEX: &str = "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex";

    fn rec(path: &str) -> ProcessRecord {
        ProcessRecord {
            uid: UID,
            pid: 1,
            path: path.to_string(),
            cpu_ns: None,
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
    fn the_second_scan_reports_busy_or_idle_and_a_wake_forgets_the_baseline() {
        let busy = |cpu_ns| ProcessRecord {
            cpu_ns: Some(cpu_ns),
            pid: 7,
            ..rec(CODEX)
        };
        let script = Rc::new(RefCell::new(vec![
            Some(vec![busy(0)]),
            Some(vec![busy(500_000_000)]),
            Some(vec![busy(500_000_000)]),
            Some(vec![busy(900_000_000)]),
        ]));
        let mut c = collector(script, Rc::new(Cell::new(0)));
        assert_eq!(
            c.collect(0).value.unwrap().states[0],
            AgentState::RunningActivityUnknown
        );
        assert_eq!(c.collect(5_000).value.unwrap().states[0], AgentState::Busy);
        assert_eq!(c.collect(10_000).value.unwrap().states[0], AgentState::Idle);
        c.on_wake();
        assert_eq!(
            c.collect(11_000).value.unwrap().states[0],
            AgentState::RunningActivityUnknown
        );
    }

    #[test]
    fn claude_follows_hook_events_for_its_running_process() {
        let claude = ProcessRecord {
            pid: 9,
            ..rec(CLAUDE)
        };
        let script = Rc::new(RefCell::new(vec![Some(vec![claude])]));
        let events = Rc::new(RefCell::new(Vec::new()));
        let feed = events.clone();
        let mut c = collector(script, Rc::new(Cell::new(0)))
            .with_hooks(Box::new(move || feed.borrow().clone()));
        assert_eq!(
            c.collect(0).value.unwrap().states[1],
            AgentState::RunningActivityUnknown
        );
        *events.borrow_mut() = vec![HookRecord {
            pid: 9,
            activity: HookActivity::Busy,
            age_ms: 0,
        }];
        assert_eq!(c.collect(5_000).value.unwrap().states[1], AgentState::Busy);
        *events.borrow_mut() = vec![HookRecord {
            pid: 9,
            activity: HookActivity::Idle,
            age_ms: 0,
        }];
        assert_eq!(c.collect(10_000).value.unwrap().states[1], AgentState::Idle);
    }

    #[test]
    fn hook_event_files_round_trip_and_dead_processes_are_pruned() {
        let dir = std::env::temp_dir().join(format!("cyd-agent-events-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let me = std::process::id() as i32;
        record_hook_event(&dir, me, HookActivity::Busy).unwrap();
        // A file left by a process that no longer exists (pid 1 always exists, so use a huge one).
        std::fs::write(dir.join(hook_file_name(999_999)), "busy\n").unwrap();
        std::fs::write(dir.join("unrelated"), "busy\n").unwrap();
        let events = read_hook_events(&dir);
        assert!(events
            .iter()
            .any(|e| e.pid == me && e.activity == HookActivity::Busy));
        assert_eq!(events.len(), 2, "the malformed name is ignored");
        record_hook_event(&dir, me, HookActivity::Idle).unwrap();
        let events = read_hook_events(&dir);
        assert_eq!(
            events.len(),
            1,
            "the dead process's file was pruned: {events:?}"
        );
        assert_eq!(events[0].activity, HookActivity::Idle);
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            std::fs::metadata(&dir).unwrap().permissions().mode() & 0o777,
            0o700
        );
        std::fs::remove_dir_all(&dir).unwrap();
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
    fn scans_failing_from_startup_become_an_error_after_15_s_at_the_normal_cadence() {
        let calls = Rc::new(Cell::new(0));
        let script = Rc::new(RefCell::new(vec![None]));
        let mut c = collector(script.clone(), calls.clone());
        for now in (0..RETAIN_MS).step_by(1_000) {
            assert_eq!(
                c.collect(now).availability,
                MetricAvailability::TemporarilyUnavailable,
                "at {now} ms"
            );
        }
        // Retried every 5 s, not on every one-second tick.
        assert_eq!(calls.get(), 3);
        assert_eq!(c.collect(RETAIN_MS).availability, MetricAvailability::Error);
        assert_eq!(
            c.collect(RETAIN_MS + 1_000).availability,
            MetricAvailability::Error
        );
        assert_eq!(calls.get(), 4);
        // The listing comes back: presence is reported at once from that scan.
        *script.borrow_mut() = vec![Some(vec![rec(CLAUDE)])];
        let back = c.collect(RETAIN_MS + 5_000).value.unwrap();
        assert_eq!(
            (back.states[1], back.age_ms),
            (AgentState::RunningActivityUnknown, 0)
        );
    }

    #[test]
    fn a_failed_refresh_retains_the_old_sample_then_reports_an_error() {
        let calls = Rc::new(Cell::new(0));
        let script = Rc::new(RefCell::new(vec![Some(vec![rec(CLAUDE)]), None]));
        let mut c = collector(script, calls.clone());
        c.collect(1_000);
        // Refresh fails: the sample stays, with its growing age, and the scan
        // is retried at the normal 5 s cadence.
        let kept = c.collect(6_000).value.unwrap();
        assert_eq!(kept.states[1], AgentState::RunningActivityUnknown);
        assert_eq!(kept.age_ms, 5_000);
        let kept = c.collect(7_000).value.unwrap();
        assert_eq!((kept.age_ms, calls.get()), (6_000, 2));
        c.collect(11_000);
        assert_eq!(calls.get(), 3);
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
        assert_eq!(c.collect(16_000).availability, MetricAvailability::Error);
        let back = c.collect(20_000).value.unwrap();
        assert_eq!((back.states, back.age_ms), ([AgentState::NotRunning; 3], 0));
    }

    #[test]
    fn a_wake_rescans_at_once_and_a_failed_rescan_shows_the_sleep() {
        let calls = Rc::new(Cell::new(0));
        let script = Rc::new(RefCell::new(vec![
            Some(vec![rec(CLAUDE)]),
            Some(vec![]),
            None,
        ]));
        let mut c = collector(script, calls.clone());
        c.collect(1_000);
        c.collect(2_000);
        assert_eq!(calls.get(), 1);
        // Woken 1 s later by the monotonic clock's reckoning (a short nap):
        // the scan runs before the next snapshot anyway.
        c.on_wake();
        let r = c.collect(3_000).value.unwrap();
        assert_eq!(
            (calls.get(), r.states, r.age_ms),
            (2, [AgentState::NotRunning; 3], 0)
        );
        // A two-hour sleep and a failing rescan: the old presence is past its
        // retention by the sleep-inclusive age, so it is an error, not shown.
        c.on_wake();
        assert_eq!(
            c.collect(3_000 + 7_200_000).availability,
            MetricAvailability::Error
        );
        assert_eq!(calls.get(), 3);
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
