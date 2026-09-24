// SPDX-License-Identifier: GPL-3.0-only

//! Battery (TB06): charge/state from the public IOPowerSources API (30 s, or at
//! once when the providing power source changes) and best-effort condition from
//! `system_profiler SPPowerDataType -json` (1 h).
//!
//! The two are independent halves with their own availability and sample age.
//! The slow half never blocks `collect`: the subprocess runs on a background
//! thread with a hard timeout (the child is killed) and an output cap, and
//! `collect` only reads the cached result, so the one-second loop never waits
//! on it. A failed refresh keeps the last good sample with its growing age;
//! the device marks it stale rather than showing a zero. A batteryless host is
//! `Absent` for both halves, never a zero charge.

use std::io::Read;
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use metrics::battery::{
    parse_power_profile, BatteryChargeReading, BatteryConditionReading, PowerProfile, ProfileError,
    MAX_PROFILER_BYTES,
};
use metrics::{Collector, MetricAvailability, MetricSample};

/// Charge is re-read this often (and immediately when the power source changes).
pub const CHARGE_REFRESH_MS: u64 = 30_000;
/// The slow condition source is re-run this often after a success.
pub const CONDITION_REFRESH_MS: u64 = 3_600_000;
/// ... and after a failure, sooner, so a transient error does not cost an hour.
pub const CONDITION_RETRY_MS: u64 = 300_000;
/// `system_profiler` is killed after this long.
pub const PROFILER_TIMEOUT: Duration = Duration::from_secs(10);
/// A run that has not reported this long after it started is abandoned (its
/// thread is left to finish on its own; its late answer is discarded).
pub const CONDITION_ABANDON_MS: u64 = 15_000;

/// One second's battery collection. Each half carries its own availability and
/// the age of its own sample.
#[derive(Debug, Clone, PartialEq)]
pub struct BatteryReading {
    pub charge: MetricSample<BatteryChargeReading>,
    pub charge_age_ms: u32,
    pub condition: MetricSample<BatteryConditionReading>,
    pub condition_age_ms: u32,
}

/// What the power-source API said about an internal battery.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChargeProbe {
    NoBattery,
    Battery(BatteryChargeReading),
    Failed,
}

/// Why a slow-source run gave no profile.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProfileFailure {
    Profile(ProfileError),
    Spawn,
    Timeout,
    ExitStatus,
}

pub type ChargeSource = Box<dyn FnMut() -> ChargeProbe>;
/// The providing power source ("AC Power", "Battery Power"), to notice a change.
pub type PowerSourceProbe = Box<dyn FnMut() -> Option<String>>;
/// The slow source: runs on a background thread and may take arbitrarily long.
pub type ProfileRunner = Arc<dyn Fn() -> Result<PowerProfile, ProfileFailure> + Send + Sync>;

#[derive(Debug, Clone, Copy)]
enum ConditionCache {
    None,
    Absent,
    Sample(BatteryConditionReading, u64),
}

#[derive(Default)]
struct Slot {
    generation: u64,
    started_ms: Option<u64>,
    done: Option<(u64, Result<PowerProfile, ProfileFailure>)>,
}

pub struct BatteryCollector {
    charge_source: ChargeSource,
    power_source: PowerSourceProbe,
    charge: Option<(ChargeProbe, u64)>,
    charge_power_source: Option<String>,
    next_charge_ms: u64,
    runner: ProfileRunner,
    slot: Arc<Mutex<Slot>>,
    condition: ConditionCache,
    condition_failure: Option<ProfileFailure>,
    next_condition_ms: u64,
}

impl BatteryCollector {
    pub fn new() -> Self {
        Self::with_sources(
            Box::new(imp::read_charge),
            Box::new(imp::providing_power_source),
            Arc::new(run_system_profiler),
        )
    }

    pub fn with_sources(
        charge_source: ChargeSource,
        power_source: PowerSourceProbe,
        runner: ProfileRunner,
    ) -> Self {
        Self {
            charge_source,
            power_source,
            charge: None,
            charge_power_source: None,
            next_charge_ms: 0,
            runner,
            slot: Arc::new(Mutex::new(Slot::default())),
            condition: ConditionCache::None,
            condition_failure: None,
            next_condition_ms: 0,
        }
    }

    fn charge(&mut self, now_ms: u64) -> (MetricSample<BatteryChargeReading>, u32) {
        let power_source = (self.power_source)();
        let changed = self.charge.is_some() && power_source != self.charge_power_source;
        if self.charge.is_none() || changed || now_ms >= self.next_charge_ms {
            self.next_charge_ms = now_ms.saturating_add(CHARGE_REFRESH_MS);
            self.charge_power_source = power_source;
            match ((self.charge_source)(), self.charge) {
                // A failed refresh keeps the last good sample and its age.
                (ChargeProbe::Failed, Some((ChargeProbe::Battery(old), at))) => {
                    self.charge = Some((ChargeProbe::Battery(old), at));
                }
                (probe, _) => self.charge = Some((probe, now_ms)),
            }
        }
        match self.charge {
            Some((ChargeProbe::Battery(reading), at)) => {
                (MetricSample::available(reading), age(now_ms, at))
            }
            Some((ChargeProbe::NoBattery, _)) => {
                (MetricSample::unavailable(MetricAvailability::Absent), 0)
            }
            _ => (MetricSample::unavailable(MetricAvailability::Error), 0),
        }
    }

    /// Applies a finished run and starts the next one when due. Never waits.
    fn condition(&mut self, now_ms: u64) -> (MetricSample<BatteryConditionReading>, u32) {
        let (finished, abandoned) = {
            let mut slot = self.slot.lock().unwrap_or_else(|e| e.into_inner());
            let finished = slot.done.take();
            let abandoned = finished.is_none()
                && slot
                    .started_ms
                    .is_some_and(|s| now_ms.saturating_sub(s) > CONDITION_ABANDON_MS);
            if finished.is_some() || abandoned {
                slot.started_ms = None;
                if abandoned {
                    slot.generation += 1; // a late answer from that run is dropped
                }
            }
            (finished, abandoned)
        };
        match finished {
            Some((started_ms, Ok(PowerProfile::Battery(reading)))) => {
                self.condition = ConditionCache::Sample(reading, started_ms);
                self.condition_failure = None;
                self.next_condition_ms = started_ms.saturating_add(CONDITION_REFRESH_MS);
            }
            Some((started_ms, Ok(PowerProfile::NoBattery))) => {
                self.condition = ConditionCache::Absent;
                self.condition_failure = None;
                self.next_condition_ms = started_ms.saturating_add(CONDITION_REFRESH_MS);
            }
            Some((_, Err(failure))) => {
                self.condition_failure = Some(failure);
                self.next_condition_ms = now_ms.saturating_add(CONDITION_RETRY_MS);
            }
            None if abandoned => {
                self.condition_failure = Some(ProfileFailure::Timeout);
                self.next_condition_ms = now_ms.saturating_add(CONDITION_RETRY_MS);
            }
            None => {}
        }
        self.start_run_if_due(now_ms);
        match self.condition {
            ConditionCache::Sample(reading, at) => {
                (MetricSample::available(reading), age(now_ms, at))
            }
            ConditionCache::Absent => (MetricSample::unavailable(MetricAvailability::Absent), 0),
            ConditionCache::None => (
                MetricSample::unavailable(match self.condition_failure {
                    None => MetricAvailability::TemporarilyUnavailable,
                    Some(ProfileFailure::Profile(
                        ProfileError::NoPowerSection | ProfileError::NoUsableFields,
                    )) => MetricAvailability::Unsupported,
                    Some(_) => MetricAvailability::Error,
                }),
                0,
            ),
        }
    }

    fn start_run_if_due(&mut self, now_ms: u64) {
        if now_ms < self.next_condition_ms {
            return;
        }
        let generation = {
            let mut slot = self.slot.lock().unwrap_or_else(|e| e.into_inner());
            if slot.started_ms.is_some() {
                return;
            }
            slot.started_ms = Some(now_ms);
            slot.generation
        };
        let runner = Arc::clone(&self.runner);
        let slot = Arc::clone(&self.slot);
        let spawned = std::thread::Builder::new()
            .name("battery-condition".into())
            .spawn(move || {
                let result = runner();
                let mut slot = slot.lock().unwrap_or_else(|e| e.into_inner());
                if slot.generation == generation {
                    slot.done = Some((now_ms, result));
                }
            });
        if spawned.is_err() {
            let mut slot = self.slot.lock().unwrap_or_else(|e| e.into_inner());
            slot.started_ms = None;
            self.condition_failure = Some(ProfileFailure::Spawn);
            self.next_condition_ms = now_ms.saturating_add(CONDITION_RETRY_MS);
        }
    }
}

fn age(now_ms: u64, sampled_ms: u64) -> u32 {
    now_ms.saturating_sub(sampled_ms).min(u32::MAX.into()) as u32
}

impl Default for BatteryCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl Collector for BatteryCollector {
    type Reading = BatteryReading;
    /// Always returns a reading; each half carries its own availability. Never
    /// blocks: the slow source runs on its own thread.
    fn collect(&mut self, now_ms: u64) -> MetricSample<BatteryReading> {
        let (charge, charge_age_ms) = self.charge(now_ms);
        let (condition, condition_age_ms) = self.condition(now_ms);
        MetricSample::available(BatteryReading {
            charge,
            charge_age_ms,
            condition,
            condition_age_ms,
        })
    }

    /// The charge is read again before the next snapshot. The hourly
    /// condition is not re-run: its sleep-inclusive age already says how old
    /// it is, and it is re-run when that age makes it due.
    fn on_wake(&mut self) {
        self.next_charge_ms = 0;
    }
}

/// Runs `program` with a hard timeout and an output cap; the child is killed on
/// timeout or when the cap is exceeded. Returns the captured stdout.
pub fn run_bounded(
    program: &str,
    args: &[&str],
    timeout: Duration,
    cap: usize,
) -> Result<Vec<u8>, ProfileFailure> {
    let mut child = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| ProfileFailure::Spawn)?;
    let mut stdout = child.stdout.take().ok_or(ProfileFailure::Spawn)?;
    let (tx, rx) = mpsc::channel();
    // Reads at most cap + 1 bytes: one byte over is enough to know it is too big.
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let _ = (&mut stdout).take(cap as u64 + 1).read_to_end(&mut bytes);
        let _ = tx.send(bytes);
    });
    let output = rx.recv_timeout(timeout);
    // Timed out, over the cap, or finished: in every case make sure it is gone.
    let _ = child.kill();
    let status = child.wait();
    match output {
        Err(_) => Err(ProfileFailure::Timeout),
        Ok(bytes) if bytes.len() > cap => Err(ProfileFailure::Profile(ProfileError::TooLarge)),
        Ok(bytes) => match status {
            Ok(s) if s.success() => Ok(bytes),
            _ => Err(ProfileFailure::ExitStatus),
        },
    }
}

/// The real slow source.
pub fn run_system_profiler() -> Result<PowerProfile, ProfileFailure> {
    let output = run_bounded(
        "/usr/sbin/system_profiler",
        &["SPPowerDataType", "-json"],
        PROFILER_TIMEOUT,
        MAX_PROFILER_BYTES,
    )?;
    parse_power_profile(&output).map_err(ProfileFailure::Profile)
}

/// The real charge read, for diagnostics.
pub fn read_charge() -> ChargeProbe {
    imp::read_charge()
}

#[cfg(target_os = "macos")]
mod imp {
    use super::ChargeProbe;
    use metrics::battery::charge_reading;
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_void};

    type CfRef = *const c_void;
    const UTF8: u32 = 0x0800_0100;
    const CF_NUMBER_SINT64: isize = 4;

    #[link(name = "IOKit", kind = "framework")]
    extern "C" {
        fn IOPSCopyPowerSourcesInfo() -> CfRef;
        fn IOPSCopyPowerSourcesList(blob: CfRef) -> CfRef;
        fn IOPSGetPowerSourceDescription(blob: CfRef, source: CfRef) -> CfRef;
        fn IOPSGetProvidingPowerSourceType(blob: CfRef) -> CfRef;
    }

    #[link(name = "CoreFoundation", kind = "framework")]
    extern "C" {
        fn CFStringCreateWithCString(alloc: CfRef, c_str: *const c_char, encoding: u32) -> CfRef;
        fn CFRelease(object: CfRef);
        fn CFGetTypeID(object: CfRef) -> usize;
        fn CFStringGetTypeID() -> usize;
        fn CFNumberGetTypeID() -> usize;
        fn CFBooleanGetTypeID() -> usize;
        fn CFDictionaryGetTypeID() -> usize;
        fn CFArrayGetCount(array: CfRef) -> isize;
        fn CFArrayGetValueAtIndex(array: CfRef, index: isize) -> CfRef;
        fn CFDictionaryGetValue(dict: CfRef, key: CfRef) -> CfRef;
        fn CFNumberGetValue(number: CfRef, kind: isize, out: *mut c_void) -> u8;
        fn CFBooleanGetValue(boolean: CfRef) -> u8;
        fn CFStringGetCString(s: CfRef, buf: *mut c_char, size: isize, encoding: u32) -> u8;
    }

    /// A Core Foundation object released on drop (a +1 Create/Copy reference).
    struct Cf(CfRef);
    impl Drop for Cf {
        fn drop(&mut self) {
            // SAFETY: a non-null owned reference from a Create/Copy call.
            unsafe { CFRelease(self.0) };
        }
    }

    fn cf_string(text: &CStr) -> Option<Cf> {
        // SAFETY: `text` is NUL-terminated; a null allocator is the default.
        let s = unsafe { CFStringCreateWithCString(std::ptr::null(), text.as_ptr(), UTF8) };
        (!s.is_null()).then(|| Cf(s))
    }

    /// The value for `key` in a CFDictionary, borrowed (Get rule).
    fn entry(dict: CfRef, key: &CStr) -> Option<CfRef> {
        let key = cf_string(key)?;
        // SAFETY: `dict` was type-checked as a CFDictionary by the caller.
        let value = unsafe { CFDictionaryGetValue(dict, key.0) };
        (!value.is_null()).then_some(value)
    }

    fn string_value(value: CfRef) -> Option<String> {
        // SAFETY: type-checked before use; the buffer size is passed.
        unsafe {
            if CFGetTypeID(value) != CFStringGetTypeID() {
                return None;
            }
            let mut buf = [0 as c_char; 96];
            if CFStringGetCString(value, buf.as_mut_ptr(), buf.len() as isize, UTF8) == 0 {
                return None;
            }
            Some(CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned())
        }
    }

    fn number_value(value: CfRef) -> Option<i64> {
        // SAFETY: type-checked before use; the out pointer is an i64.
        unsafe {
            if CFGetTypeID(value) != CFNumberGetTypeID() {
                return None;
            }
            let mut out: i64 = 0;
            (CFNumberGetValue(value, CF_NUMBER_SINT64, (&mut out as *mut i64).cast()) != 0)
                .then_some(out)
        }
    }

    fn bool_value(value: CfRef) -> Option<bool> {
        // SAFETY: type-checked before use.
        unsafe {
            (CFGetTypeID(value) == CFBooleanGetTypeID()).then(|| CFBooleanGetValue(value) != 0)
        }
    }

    pub fn providing_power_source() -> Option<String> {
        // SAFETY: the info blob is owned by `Cf`; the returned string is
        // borrowed from it and copied before the blob is released.
        unsafe {
            let info = IOPSCopyPowerSourcesInfo();
            if info.is_null() {
                return None;
            }
            let info = Cf(info);
            let kind = IOPSGetProvidingPowerSourceType(info.0);
            (!kind.is_null()).then(|| string_value(kind)).flatten()
        }
    }

    pub fn read_charge() -> ChargeProbe {
        // SAFETY: every borrowed reference below is used only while `info` and
        // `list` (both owned by `Cf`) are alive, and each is type-checked.
        unsafe {
            let info = IOPSCopyPowerSourcesInfo();
            if info.is_null() {
                return ChargeProbe::Failed;
            }
            let info = Cf(info);
            let list = IOPSCopyPowerSourcesList(info.0);
            if list.is_null() {
                return ChargeProbe::Failed;
            }
            let list = Cf(list);
            for i in 0..CFArrayGetCount(list.0) {
                let source = CFArrayGetValueAtIndex(list.0, i);
                let dict = IOPSGetPowerSourceDescription(info.0, source);
                if dict.is_null() || CFGetTypeID(dict) != CFDictionaryGetTypeID() {
                    continue;
                }
                let text = |key: &CStr| entry(dict, key).and_then(string_value);
                let number = |key: &CStr| entry(dict, key).and_then(number_value);
                let flag = |key: &CStr| entry(dict, key).and_then(bool_value);
                if text(c"Type").as_deref() != Some("InternalBattery") {
                    continue;
                }
                if flag(c"Is Present") == Some(false) {
                    return ChargeProbe::NoBattery;
                }
                let on_ac = text(c"Power Source State").map(|s| s == "AC Power");
                return match (number(c"Current Capacity"), number(c"Max Capacity")) {
                    (Some(current), Some(max)) => charge_reading(
                        current,
                        max,
                        flag(c"Is Charging"),
                        flag(c"Is Charged"),
                        on_ac,
                        number(c"Time to Empty"),
                        number(c"Time to Full Charge"),
                    )
                    .map_or(ChargeProbe::Failed, ChargeProbe::Battery),
                    _ => ChargeProbe::Failed,
                };
            }
            ChargeProbe::NoBattery
        }
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    use super::ChargeProbe;
    pub fn providing_power_source() -> Option<String> {
        None
    }
    pub fn read_charge() -> ChargeProbe {
        ChargeProbe::Failed
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use metrics::battery::{BatteryCondition, BatteryState};
    use std::cell::Cell;
    use std::rc::Rc;
    use std::time::Instant;

    fn charge(per_mille: u16) -> ChargeProbe {
        ChargeProbe::Battery(BatteryChargeReading {
            per_mille,
            state: BatteryState::Discharging,
            time_remaining_min: Some(90),
        })
    }

    fn health() -> BatteryConditionReading {
        BatteryConditionReading {
            max_capacity_per_mille: Some(910),
            condition: BatteryCondition::Normal,
            cycle_count: Some(312),
        }
    }

    fn quick_runner(result: Result<PowerProfile, ProfileFailure>) -> ProfileRunner {
        Arc::new(move || result)
    }

    /// Polls `collect` until the background result lands (bounded).
    fn settle(collector: &mut BatteryCollector, now_ms: u64) -> BatteryReading {
        for _ in 0..200 {
            std::thread::sleep(Duration::from_millis(5));
            let reading = collector.collect(now_ms).value.unwrap();
            if reading.condition.availability != MetricAvailability::TemporarilyUnavailable {
                return reading;
            }
        }
        panic!("the background run never reported");
    }

    fn fixed_charge(probe: ChargeProbe) -> (ChargeSource, PowerSourceProbe) {
        (
            Box::new(move || probe),
            Box::new(|| Some("Battery Power".to_string())),
        )
    }

    #[test]
    fn charge_is_cached_for_30_seconds_with_its_own_age() {
        let calls = Rc::new(Cell::new(0));
        let counter = Rc::clone(&calls);
        let mut c = BatteryCollector::with_sources(
            Box::new(move || {
                counter.set(counter.get() + 1);
                charge(870)
            }),
            Box::new(|| Some("Battery Power".to_string())),
            quick_runner(Ok(PowerProfile::Battery(health()))),
        );
        let r = c.collect(1000).value.unwrap();
        assert_eq!(
            (r.charge.value.unwrap().per_mille, r.charge_age_ms),
            (870, 0)
        );
        let r = c.collect(20_000).value.unwrap();
        assert_eq!(r.charge_age_ms, 19_000, "the age grows between samples");
        assert_eq!(calls.get(), 1);
        c.collect(31_000);
        assert_eq!(calls.get(), 2, "re-read after 30 s");
        assert_eq!(c.collect(31_000).value.unwrap().charge_age_ms, 0);
    }

    #[test]
    fn a_wake_rereads_the_charge_and_a_failed_reread_shows_the_sleep() {
        let reads = Rc::new(Cell::new(0u32));
        let counter = Rc::clone(&reads);
        let mut c = BatteryCollector::with_sources(
            Box::new(move || {
                counter.set(counter.get() + 1);
                match counter.get() {
                    1 => charge(800),
                    2 => charge(640),
                    _ => ChargeProbe::Failed,
                }
            }),
            Box::new(|| Some("Battery Power".to_string())),
            quick_runner(Ok(PowerProfile::NoBattery)),
        );
        assert_eq!(c.collect(1_000).value.unwrap().charge_age_ms, 0);
        c.collect(5_000);
        assert_eq!(reads.get(), 1);
        // A 10 s sleep: inside the 30 s cadence, yet the charge is re-read
        // before the next snapshot instead of re-sending 80% as current.
        c.on_wake();
        let r = c.collect(15_000).value.unwrap();
        assert_eq!(reads.get(), 2);
        assert_eq!(
            (r.charge.value.unwrap().per_mille, r.charge_age_ms),
            (640, 0)
        );
        // Two hours asleep and the re-read fails: the old charge is kept with
        // its sleep-inclusive age (stale on the device), never as fresh.
        c.on_wake();
        let r = c.collect(15_000 + 7_200_000).value.unwrap();
        assert_eq!(reads.get(), 3);
        assert_eq!(
            (r.charge.value.unwrap().per_mille, r.charge_age_ms),
            (640, 7_200_000)
        );
    }

    #[test]
    fn a_power_source_change_resamples_at_once() {
        let source = Rc::new(Cell::new("AC Power"));
        let seen = Rc::clone(&source);
        let reads = Rc::new(Cell::new(0u16));
        let counter = Rc::clone(&reads);
        let mut c = BatteryCollector::with_sources(
            Box::new(move || {
                counter.set(counter.get() + 1);
                charge(500 + counter.get())
            }),
            Box::new(move || Some(seen.get().to_string())),
            quick_runner(Ok(PowerProfile::NoBattery)),
        );
        c.collect(1000);
        c.collect(2000);
        assert_eq!(reads.get(), 1);
        source.set("Battery Power"); // unplugged
        let r = c.collect(3000).value.unwrap();
        assert_eq!(reads.get(), 2, "resampled well before the 30 s cadence");
        assert_eq!(
            (r.charge.value.unwrap().per_mille, r.charge_age_ms),
            (502, 0)
        );
    }

    #[test]
    fn a_batteryless_host_is_absent_for_both_halves_never_zero() {
        let (charge_source, power) = fixed_charge(ChargeProbe::NoBattery);
        let mut c = BatteryCollector::with_sources(
            charge_source,
            power,
            quick_runner(Ok(PowerProfile::NoBattery)),
        );
        let r = settle(&mut c, 1000);
        assert_eq!(r.charge.availability, MetricAvailability::Absent);
        assert_eq!(r.charge.value, None);
        assert_eq!(r.condition.availability, MetricAvailability::Absent);
        assert_eq!(r.condition.value, None);
    }

    #[test]
    fn a_failed_charge_read_is_an_error_first_and_keeps_the_old_sample_later() {
        let probes = Rc::new(Cell::new(0));
        let n = Rc::clone(&probes);
        let mut c = BatteryCollector::with_sources(
            Box::new(move || {
                n.set(n.get() + 1);
                if n.get() == 2 {
                    ChargeProbe::Failed
                } else {
                    charge(700)
                }
            }),
            Box::new(|| None),
            quick_runner(Ok(PowerProfile::NoBattery)),
        );
        let r = c.collect(0).value.unwrap();
        assert_eq!(r.charge.value.unwrap().per_mille, 700);
        let r = c.collect(30_000).value.unwrap();
        assert_eq!(probes.get(), 2);
        assert_eq!(
            (r.charge.value.unwrap().per_mille, r.charge_age_ms),
            (700, 30_000),
            "the failed refresh kept the old sample, aging"
        );
        let mut failing = BatteryCollector::with_sources(
            Box::new(|| ChargeProbe::Failed),
            Box::new(|| None),
            quick_runner(Ok(PowerProfile::NoBattery)),
        );
        let r = failing.collect(0).value.unwrap();
        assert_eq!(r.charge.availability, MetricAvailability::Error);
        assert_eq!(r.charge.value, None);
    }

    #[test]
    fn condition_arrives_from_the_background_thread_and_is_cached_for_an_hour() {
        let runs = Arc::new(Mutex::new(0u32));
        let counter = Arc::clone(&runs);
        let runner: ProfileRunner = Arc::new(move || {
            *counter.lock().unwrap() += 1;
            Ok(PowerProfile::Battery(health()))
        });
        let (charge_source, power) = fixed_charge(charge(870));
        let mut c = BatteryCollector::with_sources(charge_source, power, runner);
        let first = c.collect(0).value.unwrap();
        assert!(
            matches!(
                first.condition.availability,
                MetricAvailability::TemporarilyUnavailable | MetricAvailability::Available
            ),
            "not yet available is temporarily unavailable, never zero"
        );
        let r = settle(&mut c, 500);
        assert_eq!(r.condition.value, Some(health()));
        assert_eq!(r.condition_age_ms, 500, "aged from when the run started");
        let r = c.collect(CONDITION_REFRESH_MS - 1).value.unwrap();
        assert_eq!(r.condition_age_ms as u64, CONDITION_REFRESH_MS - 1);
        assert_eq!(*runs.lock().unwrap(), 1);
        c.collect(CONDITION_REFRESH_MS);
        let r = settle(&mut c, CONDITION_REFRESH_MS + 1);
        assert_eq!(*runs.lock().unwrap(), 2, "re-run after an hour");
        assert!(r.condition_age_ms < 1000);
    }

    #[test]
    fn a_failed_condition_run_keeps_the_old_sample_and_retries_sooner() {
        let results = Arc::new(Mutex::new(vec![
            Err(ProfileFailure::Timeout),
            Ok(PowerProfile::Battery(health())),
        ]));
        let queue = Arc::clone(&results);
        let runner: ProfileRunner = Arc::new(move || queue.lock().unwrap().pop().unwrap());
        let (charge_source, power) = fixed_charge(charge(870));
        let mut c = BatteryCollector::with_sources(charge_source, power, runner);
        c.collect(0);
        let r = settle(&mut c, 100);
        assert_eq!(r.condition.value, Some(health()));
        // The hourly run fails: the last good sample stays, with its age.
        c.collect(CONDITION_REFRESH_MS);
        std::thread::sleep(Duration::from_millis(50));
        let r = c.collect(CONDITION_REFRESH_MS + 100).value.unwrap();
        assert_eq!(r.condition.value, Some(health()));
        assert!(r.condition_age_ms as u64 >= CONDITION_REFRESH_MS);
    }

    #[test]
    fn failure_kinds_map_to_availability_when_there_is_no_sample() {
        for (failure, expected) in [
            (
                ProfileFailure::Profile(ProfileError::NoPowerSection),
                MetricAvailability::Unsupported,
            ),
            (
                ProfileFailure::Profile(ProfileError::NoUsableFields),
                MetricAvailability::Unsupported,
            ),
            (
                ProfileFailure::Profile(ProfileError::Malformed),
                MetricAvailability::Error,
            ),
            (
                ProfileFailure::Profile(ProfileError::TooLarge),
                MetricAvailability::Error,
            ),
            (ProfileFailure::Timeout, MetricAvailability::Error),
            (ProfileFailure::Spawn, MetricAvailability::Error),
        ] {
            let (charge_source, power) = fixed_charge(charge(870));
            let mut c =
                BatteryCollector::with_sources(charge_source, power, quick_runner(Err(failure)));
            let r = settle(&mut c, 100);
            assert_eq!(r.condition.availability, expected, "{failure:?}");
            assert_eq!(r.condition.value, None);
            assert_eq!(r.charge.availability, MetricAvailability::Available);
        }
    }

    #[test]
    fn a_hung_slow_source_never_blocks_collect() {
        let release = Arc::new(Mutex::new(()));
        let held = release.lock().unwrap(); // the fake runner blocks on this
        let gate = Arc::clone(&release);
        let runner: ProfileRunner = Arc::new(move || {
            let _wait = gate.lock().unwrap();
            Ok(PowerProfile::Battery(health()))
        });
        let (charge_source, power) = fixed_charge(charge(870));
        let mut c = BatteryCollector::with_sources(charge_source, power, runner);
        let started = Instant::now();
        for second in 0..20 {
            let r = c.collect(second * 1000).value.unwrap();
            assert_eq!(r.charge.value.unwrap().per_mille, 870, "charge stays live");
            if second < 15 {
                assert_eq!(
                    r.condition.availability,
                    MetricAvailability::TemporarilyUnavailable
                );
            }
        }
        assert!(
            started.elapsed() < Duration::from_millis(500),
            "twenty collections took {:?}",
            started.elapsed()
        );
        // Past the abandon deadline the hung run is written off as an error.
        let r = c.collect(20_000).value.unwrap();
        assert_eq!(r.condition.availability, MetricAvailability::Error);
        // Its late answer is discarded; a retry later gets a fresh result.
        drop(held);
        std::thread::sleep(Duration::from_millis(50));
        let r = c.collect(21_000).value.unwrap();
        assert_eq!(r.condition.availability, MetricAvailability::Error);
        c.collect(20_000 + CONDITION_RETRY_MS);
        let mut r = c.collect(20_000 + CONDITION_RETRY_MS).value.unwrap();
        for _ in 0..200 {
            if r.condition.value.is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(5));
            r = c.collect(20_000 + CONDITION_RETRY_MS).value.unwrap();
        }

        assert_eq!(r.condition.value, Some(health()));
    }

    #[test]
    fn run_bounded_kills_a_hung_child_at_the_timeout() {
        let started = Instant::now();
        let result = run_bounded("/bin/sleep", &["30"], Duration::from_millis(200), 1024);
        assert_eq!(result, Err(ProfileFailure::Timeout));
        assert!(started.elapsed() < Duration::from_secs(5));
    }

    #[test]
    fn run_bounded_caps_output_and_reports_a_missing_program() {
        let started = Instant::now();
        let result = run_bounded("/usr/bin/yes", &[], Duration::from_secs(5), 4096);
        assert_eq!(result, Err(ProfileFailure::Profile(ProfileError::TooLarge)));
        assert!(started.elapsed() < Duration::from_secs(4), "yes was killed");
        assert_eq!(
            run_bounded("/nonexistent/program", &[], Duration::from_secs(1), 10),
            Err(ProfileFailure::Spawn)
        );
        assert_eq!(
            run_bounded("/bin/echo", &["hello"], Duration::from_secs(5), 100),
            Ok(b"hello\n".to_vec())
        );
        assert_eq!(
            run_bounded("/usr/bin/false", &[], Duration::from_secs(5), 100),
            Err(ProfileFailure::ExitStatus)
        );
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn the_real_sources_answer_without_panicking() {
        // What this machine reports, for a person to read with --nocapture.
        let charge = read_charge();
        eprintln!("real charge probe: {charge:?}");
        assert!(
            !matches!(charge, ChargeProbe::Failed),
            "IOPowerSources answered"
        );
        eprintln!("real power source: {:?}", imp::providing_power_source());
        let profile = run_system_profiler();
        eprintln!("real profile: {profile:?}");
        if let Ok(PowerProfile::Battery(b)) = profile {
            assert!(b.max_capacity_per_mille.is_some() || b.cycle_count.is_some());
        }
    }
}
