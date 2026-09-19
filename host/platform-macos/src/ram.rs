// SPDX-License-Identifier: GPL-3.0-only

//! Physical RAM Occupied: `hw.memsize` minus Mach `free_count` pages, both
//! public sources, no root. "Free" is the kernel's free-page count, so the
//! figure includes reclaimable caches (docs/host-metrics.md); it is never
//! relabelled as application memory or pressure.

use metrics::{Collector, MetricAvailability, MetricSample, RamReading};

/// Total physical bytes and free bytes, or `None` if the kernel call fails.
pub type MemorySource = fn() -> Option<(u64, u64)>;

pub fn read_memory() -> Option<(u64, u64)> {
    imp::read_memory()
}

/// A rate needs no baseline: RAM is available on the first collection.
pub struct RamCollector {
    source: MemorySource,
}

impl RamCollector {
    pub fn new() -> Self {
        Self::with_source(read_memory)
    }
    pub fn with_source(source: MemorySource) -> Self {
        Self { source }
    }
}

impl Default for RamCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl Collector for RamCollector {
    type Reading = RamReading;
    fn collect(&mut self, _now_ms: u64) -> MetricSample<RamReading> {
        match (self.source)() {
            // Free above total (or a zero total) is a broken read, not "0 used".
            Some((total, free)) if total > 0 && free <= total => {
                MetricSample::available(RamReading {
                    used_bytes: total - free,
                    total_bytes: total,
                })
            }
            _ => MetricSample::unavailable(MetricAvailability::Error),
        }
    }
}

#[cfg(target_os = "macos")]
mod imp {
    use std::ffi::CString;
    use std::os::raw::{c_char, c_int, c_uint, c_void};

    const HOST_VM_INFO64: c_int = 4;
    // sizeof(vm_statistics64_data_t) / sizeof(integer_t) on macOS.
    const HOST_VM_INFO64_COUNT: c_uint = 38;

    extern "C" {
        fn mach_host_self() -> c_uint;
        fn host_page_size(host: c_uint, size: *mut usize) -> c_int;
        fn host_statistics64(
            host: c_uint,
            flavor: c_int,
            info: *mut c_int,
            count: *mut c_uint,
        ) -> c_int;
        fn sysctlbyname(
            name: *const c_char,
            old: *mut c_void,
            old_len: *mut usize,
            new: *mut c_void,
            new_len: usize,
        ) -> c_int;
    }

    fn total_bytes() -> Option<u64> {
        let name = CString::new("hw.memsize").ok()?;
        let mut value: u64 = 0;
        let mut len = std::mem::size_of::<u64>();
        // SAFETY: `value` is an 8-byte buffer and `len` says so.
        let status = unsafe {
            sysctlbyname(
                name.as_ptr(),
                (&mut value as *mut u64).cast(),
                &mut len,
                std::ptr::null_mut(),
                0,
            )
        };
        (status == 0 && len == 8).then_some(value)
    }

    pub fn read_memory() -> Option<(u64, u64)> {
        let mut page_size = 0usize;
        // SAFETY: `page_size` is a valid out pointer.
        if unsafe { host_page_size(mach_host_self(), &mut page_size) } != 0 || page_size == 0 {
            return None;
        }
        // The struct is 38 32-bit words; only the first, `free_count`
        // (natural_t pages), is read.
        let mut info = [0 as c_int; HOST_VM_INFO64_COUNT as usize];
        let mut count = HOST_VM_INFO64_COUNT;
        // SAFETY: `info` has room for `count` integers, what the flavor writes.
        let status = unsafe {
            host_statistics64(
                mach_host_self(),
                HOST_VM_INFO64,
                info.as_mut_ptr(),
                &mut count,
            )
        };
        if status != 0 || count != HOST_VM_INFO64_COUNT {
            return None;
        }
        let free_pages = u64::from(info[0] as u32);
        Some((total_bytes()?, free_pages * page_size as u64))
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    pub fn read_memory() -> Option<(u64, u64)> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn used_is_total_minus_free() {
        let mut ram = RamCollector::with_source(|| Some((16 << 30, 4 << 30)));
        let sample = ram.collect(0);
        assert_eq!(
            sample.value,
            Some(RamReading {
                used_bytes: 12 << 30,
                total_bytes: 16 << 30
            })
        );
        assert_eq!(sample.availability, MetricAvailability::Available);
    }

    #[test]
    fn a_failed_or_inconsistent_read_is_an_error_never_zero_used() {
        for source in [
            (|| None) as MemorySource,
            || Some((0, 0)),
            || Some((8 << 30, 9 << 30)), // free above total
        ] {
            let sample = RamCollector::with_source(source).collect(0);
            assert_eq!(sample.availability, MetricAvailability::Error);
            assert_eq!(sample.value, None);
        }
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn the_real_source_reports_plausible_memory() {
        let (total, free) = read_memory().expect("host_statistics64 and hw.memsize");
        assert!(total >= 1 << 30, "total {total}");
        assert!(free <= total, "free {free} total {total}");
        assert_eq!(total % 4096, 0);
    }
}
