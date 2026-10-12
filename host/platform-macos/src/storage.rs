// SPDX-License-Identifier: GPL-3.0-only

//! Storage: capacity of the filesystem containing `/` (30 s cache) and the
//! read/write byte rates totalled over every physical storage device (1 s).
//!
//! Capacity is `statfs("/")`, the same volume-level figure Foundation reports;
//! on APFS it is the container's shared space, so volumes are never summed and
//! purgeable space is not counted as available. I/O is the public user-space
//! IOKit `IOBlockStorageDriver` `Statistics` dictionary ("Bytes (Read)" and
//! "Bytes (Write)") of every block-storage driver, internal and external,
//! except those on a virtual device (a mounted disk image, whose traffic is
//! already counted on the disk holding the image). APFS containers are not
//! block-storage drivers, so nothing is counted twice. Each device keeps its
//! own baseline: a device that appears makes the total unavailable for one
//! sample, one that disappears simply stops counting. No root, no dependency.

use metrics::cache::{RefreshPolicy, SampleCache};
use metrics::storage::{DiskCounters, DiskIoSampler, DiskRate, StorageCapacityReading};
use metrics::{Collector, MetricAvailability, MetricSample};
use std::collections::HashMap;

/// How long a capacity sample is reused before `/` is asked again.
pub const CAPACITY_REFRESH_MS: u64 = 30_000;

/// One second's storage collection: capacity with the age of its own sample,
/// and the disk rate. Capacity and I/O availability are independent.
#[derive(Debug, Clone, PartialEq)]
pub struct StorageReading {
    pub capacity: MetricSample<StorageCapacityReading>,
    /// Milliseconds since the capacity sample was taken (0 when unavailable).
    pub capacity_age_ms: u32,
    pub io: MetricSample<DiskRate>,
}

/// Capacity of `/`, or `None` if the call fails or the numbers are impossible.
pub type CapacitySource = Box<dyn FnMut() -> Option<StorageCapacityReading>>;
/// Cumulative counters of every physical storage device, each with a stable
/// identity (a new identity is a new device with its own baseline), or `None`
/// if the devices cannot be listed.
pub type CounterSource = Box<dyn FnMut() -> Option<Vec<(String, DiskCounters)>>>;

pub struct StorageCollector {
    capacity_source: CapacitySource,
    counter_source: CounterSource,
    capacity: SampleCache<StorageCapacityReading>,
    samplers: HashMap<String, DiskIoSampler>,
}

impl StorageCollector {
    pub fn new() -> Self {
        Self::with_sources(
            Box::new(imp::read_capacity),
            Box::new(imp::physical_disk_counters),
        )
    }

    pub fn with_sources(capacity_source: CapacitySource, counter_source: CounterSource) -> Self {
        Self {
            capacity_source,
            counter_source,
            // Until the first good read, `/` is asked every second.
            capacity: SampleCache::new(RefreshPolicy {
                refresh_ms: CAPACITY_REFRESH_MS,
                retry_until_sampled: true,
            }),
            samplers: HashMap::new(),
        }
    }

    fn capacity(&mut self, now_ms: u64) -> (MetricSample<StorageCapacityReading>, u32) {
        self.capacity
            .refresh_with(now_ms, || (self.capacity_source)());
        match self.capacity.get(now_ms) {
            // A refresh that failed keeps the last sample with its growing age;
            // the device marks it stale rather than showing a zero.
            Some(cached) => (MetricSample::available(*cached.value), cached.wire_age_ms()),
            None => (MetricSample::unavailable(MetricAvailability::Error), 0),
        }
    }

    fn io(&mut self, now_ms: u64) -> MetricSample<DiskRate> {
        let disks = match (self.counter_source)() {
            Some(disks) if !disks.is_empty() => disks,
            _ => {
                self.samplers.clear();
                return MetricSample::unavailable(MetricAvailability::Error);
            }
        };
        // Every device is sampled each second, so each keeps its baseline; a
        // device without one (just attached) leaves the total unavailable.
        let mut total = Some(DiskRate {
            read_bytes_per_s: 0,
            write_bytes_per_s: 0,
        });
        let mut availability = MetricAvailability::Available;
        for (device, counters) in &disks {
            let sample = self
                .samplers
                .entry(device.clone())
                .or_default()
                .sample(*counters, now_ms);
            match (sample.value, total) {
                (Some(rate), Some(sum)) => {
                    total = Some(DiskRate {
                        read_bytes_per_s: sum
                            .read_bytes_per_s
                            .saturating_add(rate.read_bytes_per_s),
                        write_bytes_per_s: sum
                            .write_bytes_per_s
                            .saturating_add(rate.write_bytes_per_s),
                    });
                }
                (None, _) => {
                    availability = sample.availability;
                    total = None;
                }
                (Some(_), None) => {}
            }
        }
        self.samplers
            .retain(|device, _| disks.iter().any(|(d, _)| d == device));
        match total {
            Some(rate) => MetricSample::available(rate),
            None => MetricSample::unavailable(availability),
        }
    }
}

impl Default for StorageCollector {
    fn default() -> Self {
        Self::new()
    }
}

impl Collector for StorageCollector {
    type Reading = StorageReading;
    /// Always returns a reading; each half carries its own availability.
    fn collect(&mut self, now_ms: u64) -> MetricSample<StorageReading> {
        let (capacity, capacity_age_ms) = self.capacity(now_ms);
        let io = self.io(now_ms);
        MetricSample::available(StorageReading {
            capacity,
            capacity_age_ms,
            io,
        })
    }

    fn on_wake(&mut self) {
        self.capacity.expire();
        self.samplers.clear();
    }
}

/// What the real source resolved for `/`, for diagnostics.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RootDiskInfo {
    pub mounted_from: String,
    pub physical_disk: String,
    pub counters: DiskCounters,
}

/// Resolve `/` once and read its counters (None if not macOS or unreachable).
pub fn probe_root_disk() -> Option<RootDiskInfo> {
    imp::probe()
}

/// The real capacity read for `/`.
pub fn read_capacity() -> Option<StorageCapacityReading> {
    imp::read_capacity()
}

#[cfg(target_os = "macos")]
mod imp {
    use super::RootDiskInfo;
    use crate::cf::{self, Cf, CfRef};
    use metrics::storage::{
        physical_disk_name, whole_disk_name, DiskCounters, StorageCapacityReading,
    };
    use std::ffi::{CStr, CString};
    use std::os::raw::{c_char, c_int, c_void};

    type IoObject = u32;
    const MAX_ANCESTOR_DEPTH: usize = 12;

    #[repr(C)]
    struct StatFs {
        f_bsize: u32,
        f_iosize: i32,
        f_blocks: u64,
        f_bfree: u64,
        f_bavail: u64,
        f_files: u64,
        f_ffree: u64,
        f_fsid: [i32; 2],
        f_owner: u32,
        f_type: u32,
        f_flags: u32,
        f_fssubtype: u32,
        f_fstypename: [c_char; 16],
        f_mntonname: [c_char; 1024],
        f_mntfromname: [c_char; 1024],
        f_flags_ext: u32,
        f_reserved: [u32; 7],
    }

    extern "C" {
        #[cfg_attr(target_arch = "x86_64", link_name = "statfs$INODE64")]
        fn statfs(path: *const c_char, buf: *mut StatFs) -> c_int;
    }

    #[link(name = "IOKit", kind = "framework")]
    extern "C" {
        fn IOBSDNameMatching(main_port: u32, options: u32, bsd_name: *const c_char) -> *mut c_void;
        fn IOServiceMatching(name: *const c_char) -> *mut c_void;
        fn IORegistryEntryGetRegistryEntryID(entry: IoObject, id: *mut u64) -> c_int;
        fn IOServiceGetMatchingServices(
            main_port: u32,
            matching: *mut c_void,
            existing: *mut IoObject,
        ) -> c_int;
        fn IOIteratorNext(iterator: IoObject) -> IoObject;
        fn IOObjectRelease(object: IoObject) -> c_int;
        fn IOObjectConformsTo(object: IoObject, class_name: *const c_char) -> u32;
        fn IORegistryEntryGetParentIterator(
            entry: IoObject,
            plane: *const c_char,
            iterator: *mut IoObject,
        ) -> c_int;
        fn IORegistryEntryCreateCFProperty(
            entry: IoObject,
            key: CfRef,
            allocator: CfRef,
            options: u32,
        ) -> CfRef;
    }

    /// An I/O Registry object released on drop.
    struct Object(IoObject);
    impl Drop for Object {
        fn drop(&mut self) {
            // SAFETY: the handle is owned by this wrapper and released once.
            unsafe { IOObjectRelease(self.0) };
        }
    }

    fn property(entry: IoObject, key: &CStr) -> Option<Cf> {
        let key = cf::string(key)?;
        // SAFETY: valid entry and key; the result is a +1 reference or null.
        unsafe {
            Cf::owned(IORegistryEntryCreateCFProperty(
                entry,
                key.get(),
                std::ptr::null(),
                0,
            ))
        }
    }

    fn string_property(entry: IoObject, key: &CStr) -> Option<String> {
        cf::string_value::<128>(property(entry, key)?.get())
    }

    fn media_for(bsd_name: &str) -> Option<Object> {
        let name = CString::new(bsd_name).ok()?;
        let mut iterator = 0;
        // SAFETY: the matching dictionary is consumed by the call; the
        // iterator is released by `Object`.
        unsafe {
            let matching = IOBSDNameMatching(0, 0, name.as_ptr());
            if matching.is_null()
                || IOServiceGetMatchingServices(0, matching, &mut iterator) != 0
                || iterator == 0
            {
                return None;
            }
        }
        let iterator = Object(iterator);
        // SAFETY: a valid iterator.
        let first = unsafe { IOIteratorNext(iterator.0) };
        (first != 0).then(|| Object(first))
    }

    fn parents(entry: IoObject) -> Vec<Object> {
        let plane = c"IOService";
        let mut iterator = 0;
        // SAFETY: valid entry, NUL-terminated plane, valid out pointer.
        if unsafe { IORegistryEntryGetParentIterator(entry, plane.as_ptr(), &mut iterator) } != 0 {
            return Vec::new();
        }
        let iterator = Object(iterator);
        let mut found = Vec::new();
        loop {
            // SAFETY: a valid iterator.
            let next = unsafe { IOIteratorNext(iterator.0) };
            if next == 0 {
                return found;
            }
            found.push(Object(next));
        }
    }

    /// The nearest ancestor (breadth first) satisfying `wanted`.
    fn nearest_ancestor(start: IoObject, wanted: &dyn Fn(IoObject) -> bool) -> Option<Object> {
        let mut level = parents(start);
        for _ in 0..MAX_ANCESTOR_DEPTH {
            if level.is_empty() {
                return None;
            }
            if let Some(i) = level.iter().position(|o| wanted(o.0)) {
                return Some(level.swap_remove(i));
            }
            level = level.iter().flat_map(|o| parents(o.0)).collect();
        }
        None
    }

    /// The BSD name of the partition an APFS container (or any whole disk) is
    /// stored on: the nearest `IOMedia` ancestor that is on a different disk.
    fn store_of(whole_disk: &str) -> Option<String> {
        let media = media_for(whole_disk)?;
        let found = nearest_ancestor(media.0, &|o| {
            string_property(o, c"BSD Name")
                .and_then(|n| whole_disk_name(&n).map(|w| w != whole_disk))
                .unwrap_or(false)
        })?;
        string_property(found.0, c"BSD Name")
    }

    fn driver_for(physical_disk: &str) -> Option<Object> {
        let media = media_for(physical_disk)?;
        nearest_ancestor(media.0, &|o| {
            // SAFETY: valid object, NUL-terminated class name.
            unsafe { IOObjectConformsTo(o, c"IOBlockStorageDriver".as_ptr()) != 0 }
        })
    }

    fn statistic(statistics: CfRef, key: &CStr) -> Option<u64> {
        let value = cf::i64_value(cf::dictionary_value(statistics, key)?)?;
        (value >= 0).then_some(value as u64)
    }

    fn read_statistics(driver: IoObject) -> Option<DiskCounters> {
        let statistics = property(driver, c"Statistics")?;
        if !cf::is_dictionary(statistics.get()) {
            return None;
        }
        Some(DiskCounters {
            read_bytes: statistic(statistics.get(), c"Bytes (Read)")?,
            write_bytes: statistic(statistics.get(), c"Bytes (Write)")?,
        })
    }

    fn root_statfs() -> Option<StatFs> {
        let mut buf = std::mem::MaybeUninit::<StatFs>::zeroed();
        // SAFETY: a valid path and a buffer of the platform's struct statfs.
        let status = unsafe { statfs(c"/".as_ptr(), buf.as_mut_ptr()) };
        // SAFETY: zero-initialised, and statfs filled it on success.
        (status == 0).then(|| unsafe { buf.assume_init() })
    }

    pub fn read_capacity() -> Option<StorageCapacityReading> {
        let fs = root_statfs()?;
        let block = u64::from(fs.f_bsize);
        let total = fs.f_blocks.checked_mul(block)?;
        let available = fs.f_bavail.checked_mul(block)?;
        // Available above total (or a zero total) is a broken read, not "0 used".
        (total > 0 && available <= total).then_some(StorageCapacityReading {
            total_bytes: total,
            available_bytes: available,
        })
    }

    fn mounted_from() -> Option<String> {
        let fs = root_statfs()?;
        // SAFETY: the kernel NUL-terminates f_mntfromname.
        let name = unsafe { CStr::from_ptr(fs.f_mntfromname.as_ptr()) };
        Some(name.to_string_lossy().into_owned())
    }

    /// Resolves `/` to its physical whole disk and the block-storage driver.
    fn resolve() -> Option<(String, String, Object)> {
        let from = mounted_from()?;
        let physical = physical_disk_name(&from, &store_of)?;
        let driver = driver_for(&physical)?;
        Some((from, physical, driver))
    }

    pub fn probe() -> Option<RootDiskInfo> {
        let (mounted_from, physical_disk, driver) = resolve()?;
        Some(RootDiskInfo {
            mounted_from,
            physical_disk,
            counters: read_statistics(driver.0)?,
        })
    }

    /// At most this many storage devices are read per second.
    const MAX_DISKS: usize = 32;

    /// Whether a block-storage driver sits on a virtual device (a mounted disk
    /// image): its device's Protocol Characteristics say so.
    fn is_virtual(driver: IoObject) -> bool {
        parents(driver).iter().any(|device| {
            let Some(characteristics) = property(device.0, c"Protocol Characteristics") else {
                return false;
            };
            if !cf::is_dictionary(characteristics.get()) {
                return false;
            }
            // A mounted disk image says "Virtual Interface" (verified with
            // `hdiutil attach`; its location is "File").
            cf::dictionary_value(characteristics.get(), c"Physical Interconnect")
                .and_then(cf::string_value::<64>)
                .as_deref()
                == Some("Virtual Interface")
        })
    }

    /// The counters of every physical block-storage driver, each keyed by its
    /// I/O Registry entry ID (stable while the device is attached). `None` when
    /// the drivers cannot be listed.
    pub fn physical_disk_counters() -> Option<Vec<(String, DiskCounters)>> {
        let mut iterator = 0;
        // SAFETY: the matching dictionary is consumed by the call; the
        // iterator is released by `Object`.
        unsafe {
            let matching = IOServiceMatching(c"IOBlockStorageDriver".as_ptr());
            if matching.is_null()
                || IOServiceGetMatchingServices(0, matching, &mut iterator) != 0
                || iterator == 0
            {
                return None;
            }
        }
        let iterator = Object(iterator);
        let mut disks = Vec::new();
        loop {
            // SAFETY: a valid iterator.
            let next = unsafe { IOIteratorNext(iterator.0) };
            if next == 0 {
                break;
            }
            let driver = Object(next);
            if disks.len() >= MAX_DISKS || is_virtual(driver.0) {
                continue;
            }
            let mut id = 0u64;
            // SAFETY: a valid entry and out pointer.
            if unsafe { IORegistryEntryGetRegistryEntryID(driver.0, &mut id) } != 0 {
                continue;
            }
            if let Some(counters) = read_statistics(driver.0) {
                disks.push((format!("{id:x}"), counters));
            }
        }
        Some(disks)
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    use super::RootDiskInfo;
    use metrics::storage::{DiskCounters, StorageCapacityReading};

    pub fn read_capacity() -> Option<StorageCapacityReading> {
        None
    }
    pub fn probe() -> Option<RootDiskInfo> {
        None
    }
    pub fn physical_disk_counters() -> Option<Vec<(String, DiskCounters)>> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::rc::Rc;

    fn cap(total: u64, available: u64) -> StorageCapacityReading {
        StorageCapacityReading {
            total_bytes: total,
            available_bytes: available,
        }
    }

    fn counters(read: u64, write: u64, device: &str) -> Option<Vec<(String, DiskCounters)>> {
        Some(vec![(
            device.to_string(),
            DiskCounters {
                read_bytes: read,
                write_bytes: write,
            },
        )])
    }

    #[test]
    fn capacity_is_cached_for_30_seconds_with_its_own_age() {
        let calls = Rc::new(RefCell::new(0u32));
        let seen = calls.clone();
        let mut c = StorageCollector::with_sources(
            Box::new(move || {
                *seen.borrow_mut() += 1;
                Some(cap(500, 200 + u64::from(*seen.borrow())))
            }),
            Box::new(|| counters(0, 0, "disk0")),
        );
        let first = c.collect(1000).value.unwrap();
        assert_eq!(first.capacity_age_ms, 0);
        let later = c.collect(20_000).value.unwrap();
        assert_eq!(*calls.borrow(), 1, "no re-read inside 30 s");
        assert_eq!(later.capacity_age_ms, 19_000);
        assert_eq!(later.capacity.value, Some(cap(500, 201)));
        let refreshed = c.collect(31_000).value.unwrap();
        assert_eq!(*calls.borrow(), 2);
        assert_eq!(refreshed.capacity_age_ms, 0);
        assert_eq!(refreshed.capacity.value, Some(cap(500, 202)));
    }

    #[test]
    fn a_failed_capacity_read_is_an_error_first_and_keeps_the_old_sample_later() {
        let ok = Rc::new(RefCell::new(false));
        let flag = ok.clone();
        let mut c = StorageCollector::with_sources(
            Box::new(move || flag.borrow().then(|| cap(500, 100))),
            Box::new(|| counters(0, 0, "disk0")),
        );
        let none = c.collect(0).value.unwrap();
        assert_eq!(none.capacity.availability, MetricAvailability::Error);
        assert_eq!(none.capacity.value, None);
        *ok.borrow_mut() = true;
        assert!(c.collect(1000).value.unwrap().capacity.value.is_some());
        *ok.borrow_mut() = false;
        let kept = c.collect(31_000).value.unwrap();
        assert_eq!(kept.capacity.value, Some(cap(500, 100)));
        assert_eq!(kept.capacity_age_ms, 30_000, "the old sample keeps ageing");
    }

    #[test]
    fn a_wake_rereads_capacity_and_rebaselines_io_and_a_failed_reread_shows_the_sleep() {
        let calls = Rc::new(RefCell::new(0u32));
        let seen = calls.clone();
        let fail = Rc::new(RefCell::new(false));
        let failing = fail.clone();
        let mut n = 0u64;
        let mut c = StorageCollector::with_sources(
            Box::new(move || {
                *seen.borrow_mut() += 1;
                (!*failing.borrow()).then(|| cap(500, 200))
            }),
            Box::new(move || {
                n += 1;
                counters(n * 1_000, n * 1_000, "disk0")
            }),
        );
        c.collect(1_000);
        assert!(c.collect(2_000).value.unwrap().io.value.is_some());
        // A 10 s sleep: shorter than the 30 s capacity cadence, but the
        // capacity is read again at once and the rate starts over.
        c.on_wake();
        let woke = c.collect(12_000).value.unwrap();
        assert_eq!(*calls.borrow(), 2, "capacity re-read on wake");
        assert_eq!(woke.capacity_age_ms, 0);
        assert_eq!(
            woke.io.availability,
            MetricAvailability::TemporarilyUnavailable
        );
        // After a two-hour sleep a failed re-read keeps the old sample aged by
        // the sleep, so the device shows it stale rather than seconds old.
        *fail.borrow_mut() = true;
        c.on_wake();
        let late = c.collect(12_000 + 7_200_000).value.unwrap();
        assert_eq!(late.capacity.value, Some(cap(500, 200)));
        assert_eq!(late.capacity_age_ms, 7_200_000);
        assert_eq!(late.io.value, None);
    }

    #[test]
    fn io_needs_a_baseline_and_capacity_stays_available_meanwhile() {
        let mut n = 0u64;
        let mut c = StorageCollector::with_sources(
            Box::new(|| Some(cap(500, 200))),
            Box::new(move || {
                n += 1;
                counters(n * 1_000_000, n * 500_000, "disk0")
            }),
        );
        let first = c.collect(0).value.unwrap();
        assert_eq!(first.capacity.availability, MetricAvailability::Available);
        assert_eq!(
            first.io.availability,
            MetricAvailability::TemporarilyUnavailable
        );
        let second = c.collect(1000).value.unwrap();
        assert_eq!(
            second.io.value,
            Some(DiskRate {
                read_bytes_per_s: 1_000_000,
                write_bytes_per_s: 500_000
            })
        );
    }

    #[test]
    fn a_new_backing_device_or_lost_source_rebaselines() {
        let script = Rc::new(RefCell::new(None));
        let feed = script.clone();
        let mut c = StorageCollector::with_sources(
            Box::new(|| Some(cap(500, 200))),
            Box::new(move || feed.borrow().clone()),
        );
        *script.borrow_mut() = counters(1_000, 1_000, "disk0");
        c.collect(0);
        *script.borrow_mut() = counters(3_000, 1_000, "disk0");
        assert!(c.collect(1000).value.unwrap().io.value.is_some());
        // Another device with smaller counters is not a "decrease" but a new baseline.
        *script.borrow_mut() = counters(9_000_000, 9_000_000, "disk4");
        assert_eq!(c.collect(2000).value.unwrap().io.value, None);
        *script.borrow_mut() = counters(9_001_000, 9_000_000, "disk4");
        assert_eq!(
            c.collect(3000)
                .value
                .unwrap()
                .io
                .value
                .unwrap()
                .read_bytes_per_s,
            1_000
        );
        // The source failing is an error and forgets the baseline.
        *script.borrow_mut() = None;
        let lost = c.collect(4000).value.unwrap();
        assert_eq!(lost.io.availability, MetricAvailability::Error);
        assert_eq!(lost.capacity.availability, MetricAvailability::Available);
        *script.borrow_mut() = counters(9_002_000, 9_000_000, "disk4");
        assert_eq!(c.collect(5000).value.unwrap().io.value, None);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn the_real_root_reports_plausible_capacity() {
        let capacity = read_capacity().expect("statfs on /");
        assert!(capacity.total_bytes >= 1 << 30, "{capacity:?}");
        assert!(capacity.available_bytes <= capacity.total_bytes);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn the_real_io_source_resolves_root_to_a_physical_driver() {
        let info = probe_root_disk().expect("IOKit IOBlockStorageDriver statistics for /");
        println!("storage probe: {info:?}");
        println!("storage capacity: {:?}", read_capacity());
        assert!(info.mounted_from.starts_with("/dev/disk"), "{info:?}");
        assert!(metrics::storage::whole_disk_name(&info.physical_disk).is_some());
        // Every physical disk is listed, the root's among them, and counters
        // never go backwards between reads.
        let first = imp::physical_disk_counters().expect("block-storage drivers");
        let second = imp::physical_disk_counters().expect("block-storage drivers");
        println!("physical disks: {}", first.len());
        assert!(!first.is_empty());
        for (device, before) in &first {
            if let Some((_, after)) = second.iter().find(|(d, _)| d == device) {
                assert!(
                    after.read_bytes >= before.read_bytes
                        && after.write_bytes >= before.write_bytes
                );
            }
        }
    }

    fn disks(list: &[(&str, u64, u64)]) -> Option<Vec<(String, DiskCounters)>> {
        Some(
            list.iter()
                .map(|(device, read, write)| {
                    (
                        device.to_string(),
                        DiskCounters {
                            read_bytes: *read,
                            write_bytes: *write,
                        },
                    )
                })
                .collect(),
        )
    }

    #[test]
    fn io_is_the_total_over_every_physical_disk() {
        let script = Rc::new(RefCell::new(disks(&[
            ("internal", 0, 0),
            ("external", 0, 0),
        ])));
        let feed = script.clone();
        let mut c = StorageCollector::with_sources(
            Box::new(|| Some(cap(500, 100))),
            Box::new(move || feed.borrow().clone()),
        );
        c.collect(0);
        *script.borrow_mut() = disks(&[("internal", 1_000, 500), ("external", 4_000, 8_000)]);
        let io = c.collect(1000).value.unwrap().io;
        assert_eq!(
            io.value,
            Some(DiskRate {
                read_bytes_per_s: 5_000,
                write_bytes_per_s: 8_500
            })
        );
    }

    #[test]
    fn an_attached_disk_needs_a_baseline_and_a_removed_one_stops_counting() {
        let script = Rc::new(RefCell::new(disks(&[("internal", 0, 0)])));
        let feed = script.clone();
        let mut c = StorageCollector::with_sources(
            Box::new(|| Some(cap(500, 100))),
            Box::new(move || feed.borrow().clone()),
        );
        c.collect(0);
        *script.borrow_mut() = disks(&[("internal", 1_000, 0)]);
        assert!(c.collect(1000).value.unwrap().io.value.is_some());
        // A USB disk plugged in: no baseline for it yet, so no total this second.
        *script.borrow_mut() = disks(&[("internal", 2_000, 0), ("usb", 70_000, 0)]);
        let attached = c.collect(2000).value.unwrap().io;
        assert_eq!(
            (attached.availability, attached.value),
            (MetricAvailability::TemporarilyUnavailable, None)
        );
        *script.borrow_mut() = disks(&[("internal", 3_000, 0), ("usb", 90_000, 0)]);
        assert_eq!(
            c.collect(3000)
                .value
                .unwrap()
                .io
                .value
                .unwrap()
                .read_bytes_per_s,
            21_000
        );
        // Ejected: the internal disk's total carries on.
        *script.borrow_mut() = disks(&[("internal", 4_000, 0)]);
        assert_eq!(
            c.collect(4000)
                .value
                .unwrap()
                .io
                .value
                .unwrap()
                .read_bytes_per_s,
            1_000
        );
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn the_real_collector_yields_capacity_then_a_rate_on_the_second_sample() {
        let mut collector = StorageCollector::new();
        let first = collector.collect(0).value.unwrap();
        assert_eq!(first.capacity.availability, MetricAvailability::Available);
        assert_eq!(first.io.value, None, "the first I/O sample is a baseline");
        std::thread::sleep(std::time::Duration::from_millis(1000));
        let second = collector.collect(1000).value.unwrap();
        println!("storage collector: {second:?}");
        assert_eq!(second.io.availability, MetricAvailability::Available);
        assert_eq!(second.capacity_age_ms, 1000);
    }
}
