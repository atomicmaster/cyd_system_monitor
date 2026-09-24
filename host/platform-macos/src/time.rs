// SPDX-License-Identifier: GPL-3.0-only

//! The Mac's local offset from UTC, so the display can show local time, and
//! the daemon's monotonic clock.

use std::os::raw::{c_char, c_int, c_long};

/// macOS `struct tm`, including the BSD `tm_gmtoff` and `tm_zone` members.
#[repr(C)]
struct Tm {
    tm_sec: c_int,
    tm_min: c_int,
    tm_hour: c_int,
    tm_mday: c_int,
    tm_mon: c_int,
    tm_year: c_int,
    tm_wday: c_int,
    tm_yday: c_int,
    tm_isdst: c_int,
    tm_gmtoff: c_long,
    tm_zone: *const c_char,
}

extern "C" {
    fn time(out: *mut i64) -> i64;
    fn localtime_r(time: *const i64, result: *mut Tm) -> *mut Tm;
}

/// Local offset from UTC in minutes east (daylight saving included), or
/// `None` if the system cannot say. Called each second by the session so a
/// daylight-saving change or a timezone move reaches the display.
pub fn local_offset_minutes() -> Option<i16> {
    let mut tm = Tm {
        tm_sec: 0,
        tm_min: 0,
        tm_hour: 0,
        tm_mday: 0,
        tm_mon: 0,
        tm_year: 0,
        tm_wday: 0,
        tm_yday: 0,
        tm_isdst: 0,
        tm_gmtoff: 0,
        tm_zone: std::ptr::null(),
    };
    // SAFETY: `time` accepts a null out pointer; `localtime_r` fills the
    // caller-owned `tm` (layout matches macOS `struct tm`) and returns null on
    // failure.
    let filled = unsafe {
        let now = time(std::ptr::null_mut());
        !localtime_r(&now, &mut tm).is_null()
    };
    if !filled {
        return None;
    }
    i16::try_from(tm.tm_gmtoff / 60)
        .ok()
        .filter(|m| (-1440..=1440).contains(m))
}

/// A shareable monotonic clock in milliseconds (the daemon's `Clock`, or a
/// test's): the one every sample age is measured on.
pub type NowMs = std::sync::Arc<dyn Fn() -> u64 + Send + Sync>;

/// Milliseconds on a monotonic clock that keeps counting while the Mac
/// sleeps, from an arbitrary origin. Every sample age and staleness limit in
/// the daemon is measured on it, so a reading taken before a sleep is as old
/// after wake as the sleep was long. (`std::time::Instant` is
/// `CLOCK_UPTIME_RAW` on macOS and stops during sleep.)
pub fn monotonic_ms() -> u64 {
    imp::monotonic_ns() / 1_000_000
}

#[cfg(target_os = "macos")]
mod imp {
    /// `CLOCK_MONOTONIC_RAW` from `<time.h>`.
    const CLOCK_MONOTONIC_RAW: u32 = 4;

    extern "C" {
        fn clock_gettime_nsec_np(clock_id: u32) -> u64;
    }

    pub fn monotonic_ns() -> u64 {
        // MONOTONIC_RAW is mach_continuous_time: it counts sleep (UPTIME_RAW
        // does not) and, unlike CLOCK_MONOTONIC, is never slewed by NTP.
        // SAFETY: no pointers; returns 0 only for an invalid clock id.
        unsafe { clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) }
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    use std::sync::OnceLock;
    use std::time::Instant;

    /// Elsewhere only a stand-in: `Instant` (which may stop during sleep).
    pub fn monotonic_ns() -> u64 {
        static ORIGIN: OnceLock<Instant> = OnceLock::new();
        ORIGIN.get_or_init(Instant::now).elapsed().as_nanos() as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_monotonic_clock_runs_forward_at_real_speed() {
        let (start, instant) = (monotonic_ms(), std::time::Instant::now());
        assert!(start > 0, "the clock id is valid");
        std::thread::sleep(std::time::Duration::from_millis(120));
        let (ours, theirs) = (monotonic_ms() - start, instant.elapsed().as_millis() as u64);
        // No sleep happened here, so it agrees with the uptime clock.
        assert!(ours.abs_diff(theirs) <= 20, "{ours} vs {theirs}");
        assert!(monotonic_ms() >= start + ours);
    }

    #[test]
    fn the_offset_matches_the_date_command() {
        let offset = local_offset_minutes().expect("localtime_r");
        let output = std::process::Command::new("date")
            .arg("+%z")
            .output()
            .unwrap();
        let text = String::from_utf8(output.stdout).unwrap();
        let text = text.trim();
        let sign = if text.starts_with('-') { -1 } else { 1 };
        let hours: i16 = text[1..3].parse().unwrap();
        let minutes: i16 = text[3..5].parse().unwrap();
        assert_eq!(offset, sign * (hours * 60 + minutes), "date said {text}");
    }
}
