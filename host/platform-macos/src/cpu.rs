// SPDX-License-Identifier: GPL-3.0-only

//! CPU load from the public Mach `host_statistics(HOST_CPU_LOAD_INFO)`
//! call. No root helper is involved.

use metrics::{Collector, CpuSampler, CpuTicks, MetricAvailability, MetricSample};

/// Read cumulative CPU ticks, or `None` if the kernel call fails or this
/// is not macOS.
pub fn read_ticks() -> Option<CpuTicks> {
    imp::read_ticks()
}

/// The CPU collector: Mach tick counters through `CpuSampler`. The tick source
/// is a plain function so tests can script it.
pub struct CpuCollector {
    source: fn() -> Option<CpuTicks>,
    sampler: CpuSampler,
}

impl CpuCollector {
    pub fn new() -> Self {
        Self::with_source(read_ticks)
    }
    pub fn with_source(source: fn() -> Option<CpuTicks>) -> Self {
        Self {
            source,
            sampler: CpuSampler::default(),
        }
    }
}

impl Default for CpuCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl Collector for CpuCollector {
    type Reading = u16;
    /// Per mille; a failed read is an explicit error, never a zero.
    fn collect(&mut self, now_ms: u64) -> MetricSample<u16> {
        match (self.source)() {
            Some(ticks) => self.sampler.sample(ticks, now_ms),
            None => {
                self.sampler.reset();
                MetricSample::unavailable(MetricAvailability::Error)
            }
        }
    }

    fn on_wake(&mut self) {
        self.sampler.reset();
    }
}

#[cfg(test)]
mod collector_tests {
    use super::*;

    #[test]
    fn a_failed_read_is_an_error_and_forces_a_new_baseline() {
        let mut cpu = CpuCollector::with_source(|| None);
        let failed = cpu.collect(0);
        assert_eq!(failed.availability, MetricAvailability::Error);
        assert_eq!(failed.value, None);
    }

    #[test]
    fn the_collector_yields_a_rate_from_two_reads() {
        use std::sync::atomic::{AtomicU32, Ordering};
        static CALLS: AtomicU32 = AtomicU32::new(0);
        fn ticks() -> Option<CpuTicks> {
            let n = CALLS.fetch_add(1, Ordering::SeqCst);
            Some(CpuTicks {
                user: 30 * n,
                system: 0,
                idle: 70 * n,
                nice: 0,
            })
        }
        let mut cpu = CpuCollector::with_source(ticks);
        assert_eq!(cpu.collect(0).value, None, "first sample is a baseline");
        assert_eq!(cpu.collect(1000).value, Some(300));
        cpu.on_wake();
        assert_eq!(cpu.collect(2000).value, None, "no rate across a sleep");
        assert_eq!(cpu.collect(3000).value, Some(300));
    }
}

#[cfg(target_os = "macos")]
mod imp {
    use super::CpuTicks;
    use std::os::raw::{c_int, c_uint};

    const HOST_CPU_LOAD_INFO: c_int = 3;
    const CPU_STATE_USER: usize = 0;
    const CPU_STATE_SYSTEM: usize = 1;
    const CPU_STATE_IDLE: usize = 2;
    const CPU_STATE_NICE: usize = 3;
    const CPU_STATE_MAX: usize = 4;

    extern "C" {
        fn mach_host_self() -> c_uint;
        fn host_statistics(
            host: c_uint,
            flavor: c_int,
            info: *mut c_int,
            count: *mut c_uint,
        ) -> c_int;
    }

    pub fn read_ticks() -> Option<CpuTicks> {
        let mut info = [0 as c_int; CPU_STATE_MAX];
        let mut count = CPU_STATE_MAX as c_uint;
        // SAFETY: `info` holds `count` integers, the size HOST_CPU_LOAD_INFO
        // writes; `mach_host_self` has no preconditions.
        let status = unsafe {
            host_statistics(
                mach_host_self(),
                HOST_CPU_LOAD_INFO,
                info.as_mut_ptr(),
                &mut count,
            )
        };
        if status != 0 || count as usize != CPU_STATE_MAX {
            return None;
        }
        Some(CpuTicks {
            user: info[CPU_STATE_USER] as u32,
            system: info[CPU_STATE_SYSTEM] as u32,
            idle: info[CPU_STATE_IDLE] as u32,
            nice: info[CPU_STATE_NICE] as u32,
        })
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    use super::CpuTicks;

    pub fn read_ticks() -> Option<CpuTicks> {
        None
    }
}

#[cfg(all(test, target_os = "macos"))]
mod tests {
    #[test]
    fn mach_reports_ticks() {
        let a = super::read_ticks().expect("host_statistics");
        assert!(a.user as u64 + a.system as u64 + a.idle as u64 > 0);
    }
}
