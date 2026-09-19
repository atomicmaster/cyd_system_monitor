// SPDX-License-Identifier: GPL-3.0-only

//! CPU load from the public Mach `host_statistics(HOST_CPU_LOAD_INFO)`
//! call. No root helper is involved.

use metrics::CpuTicks;

/// Read cumulative CPU ticks, or `None` if the kernel call fails or this
/// is not macOS.
pub fn read_ticks() -> Option<CpuTicks> {
    imp::read_ticks()
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
