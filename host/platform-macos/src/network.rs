// SPDX-License-Identifier: GPL-3.0-only

//! The macOS view of the network for TB05: interfaces and Host Station
//! Identity from `getifaddrs`, 64-bit byte counters from the per-interface
//! `net.link.generic.ifdata` sysctl (`IFMIB_IFDATA`, the source `netstat -ib`
//! uses; `NET_RT_IFLIST2` was measured on macOS 26 to return byte counts cut
//! to 32 bits and 1 KiB steps, which would quantize and wrap the rates),
//! default routes from the routing table dump
//! (`NET_RT_DUMP`), and each interface's type from SystemConfiguration. All
//! public APIs, no root, no dependency, and no CoreWLAN: band/channel and
//! SSID/BSSID come only through the provider seam
//! (`metrics::wifi_context`, `docs/wifi-context-provider.md`).
//!
//! The byte-level parsers are pure functions over buffers and are tested with
//! hand-built messages; the FFI is a thin shell around them.

use std::collections::HashMap;

use metrics::network::{
    DefaultRoute, InterfaceCounters, InterfaceInfo, LinkKind, NetworkCollector, NetworkSystem,
    SystemView,
};
use metrics::wifi_context::{TrackerProvider, WifiContextProvider};

/// A collector over the real system and the given provider (the socket
/// server's tracker in the daemon, a fixture in tests and scenarios).
pub fn collector(provider: Box<dyn WifiContextProvider>) -> NetworkCollector {
    NetworkCollector::new(Box::new(MacNetworkSystem::default()), provider)
}

/// A collector whose provider is the shared tracker of a running server.
pub fn collector_with_tracker(provider: TrackerProvider) -> NetworkCollector {
    collector(Box::new(provider))
}

// ---------------------------------------------------------------------------
// Pure parsers
// ---------------------------------------------------------------------------

const RTF_UP: i32 = 0x1;
const RTF_REJECT: i32 = 0x8;
const RTF_BLACKHOLE: i32 = 0x1000;
const RTF_IFSCOPE: i32 = 0x0100_0000;
const RTA_DST: i32 = 0x1;
const RTA_NETMASK: i32 = 0x4;
const RTAX_MAX: usize = 8;
const AF_INET: u8 = 2;
const AF_INET6: u8 = 30;
const AF_LINK: u8 = 18;
/// `sizeof(struct rt_msghdr)` on Darwin.
const RT_MSGHDR_LEN: usize = 92;
/// Offsets of the 64-bit counters inside `if_data64` (`ifi_ibytes`, `ifi_obytes`).
const DATA64_IBYTES: usize = 64;
const DATA64_OBYTES: usize = 72;
/// Longest routing message accepted; larger is a corrupt buffer.
const MAX_MSG_LEN: usize = 8192;

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_ne_bytes(b.get(at..at + 2)?.try_into().ok()?))
}
fn i32_at(b: &[u8], at: usize) -> Option<i32> {
    Some(i32::from_ne_bytes(b.get(at..at + 4)?.try_into().ok()?))
}
fn u64_at(b: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_ne_bytes(b.get(at..at + 8)?.try_into().ok()?))
}

/// Iterates the routing messages of a sysctl buffer, stopping at the first
/// malformed length.
fn messages(buf: &[u8]) -> impl Iterator<Item = &[u8]> {
    let mut at = 0;
    std::iter::from_fn(move || {
        let len = usize::from(u16_at(buf, at)?);
        if !(4..=MAX_MSG_LEN).contains(&len) || at + len > buf.len() {
            return None;
        }
        let message = &buf[at..at + len];
        at += len;
        Some(message)
    })
}

/// `struct ifmibdata`: 16-byte name, then five 32-bit fields and four filler
/// words, then `if_data64`.
const IFMIB_DATA: usize = 52;
/// `sizeof(struct ifmibdata)`.
const IFMIB_LEN: usize = IFMIB_DATA + 128;

/// The 64-bit cumulative byte counters in an `IFMIB_IFDATA`/`IFDATA_GENERAL`
/// reply.
pub fn parse_ifmib(buf: &[u8]) -> Option<InterfaceCounters> {
    if buf.len() < IFMIB_LEN {
        return None;
    }
    Some(InterfaceCounters {
        rx_bytes: u64_at(buf, IFMIB_DATA + DATA64_IBYTES)?,
        tx_bytes: u64_at(buf, IFMIB_DATA + DATA64_OBYTES)?,
    })
}

/// Default routes (interface index and whether interface-scoped) from a
/// `NET_RT_DUMP` buffer, in table order. A default route is a destination of
/// all zeros with an all-zero or absent netmask, up, and neither reject nor
/// blackhole.
pub fn parse_default_routes(buf: &[u8]) -> Vec<(u32, bool)> {
    let mut out = Vec::new();
    for m in messages(buf) {
        let (Some(index), Some(flags), Some(addrs)) = (u16_at(m, 4), i32_at(m, 8), i32_at(m, 12))
        else {
            continue;
        };
        if m.len() < RT_MSGHDR_LEN
            || flags & RTF_UP == 0
            || flags & (RTF_REJECT | RTF_BLACKHOLE) != 0
            || addrs & RTA_DST == 0
        {
            continue;
        }
        // Socket addresses follow the header in RTAX order, each padded to 4.
        let mut at = RT_MSGHDR_LEN;
        let mut socket_addresses: [Option<&[u8]>; RTAX_MAX] = [None; RTAX_MAX];
        let mut ok = true;
        for (bit, slot) in socket_addresses.iter_mut().enumerate() {
            if addrs & (1 << bit) == 0 {
                continue;
            }
            let Some(&len) = m.get(at) else {
                ok = false;
                break;
            };
            let len = usize::from(len);
            let advance = if len == 0 { 4 } else { (len + 3) & !3 };
            let Some(sa) = m.get(at..(at + len.max(1)).min(m.len())) else {
                ok = false;
                break;
            };
            *slot = Some(sa);
            at += advance;
        }
        if !ok {
            continue;
        }
        let dst = socket_addresses[0].expect("RTA_DST checked");
        let zero_from = |sa: &[u8], from: usize| sa.iter().skip(from).all(|&b| b == 0);
        let dst_is_default = match dst.get(1).copied() {
            Some(AF_INET) => zero_from(dst, 4),
            Some(AF_INET6) => zero_from(dst, 8),
            _ => false,
        };
        let mask_is_zero = if addrs & RTA_NETMASK == 0 {
            true
        } else {
            let mask = socket_addresses[2].unwrap_or(&[]);
            // An IPv4/IPv6 netmask's address bytes follow a 4/8 byte header.
            zero_from(mask, if dst.get(1) == Some(&AF_INET6) { 8 } else { 4 })
        };
        if dst_is_default && mask_is_zero {
            out.push((u32::from(index), flags & RTF_IFSCOPE != 0));
        }
    }
    out
}

/// The link-layer address of an `AF_LINK` socket address (`sockaddr_dl`), when
/// it is a 6-byte, non-zero address.
pub fn parse_link_address(sa: &[u8]) -> Option<[u8; 6]> {
    if sa.get(1) != Some(&AF_LINK) {
        return None;
    }
    let name_len = usize::from(*sa.get(5)?);
    let addr_len = usize::from(*sa.get(6)?);
    if addr_len != 6 {
        return None;
    }
    let mac: [u8; 6] = sa.get(8 + name_len..8 + name_len + 6)?.try_into().ok()?;
    (mac != [0; 6]).then_some(mac)
}

/// Whether an interface name is a tunnel over some other interface.
pub fn is_tunnel_name(name: &str) -> bool {
    ["utun", "ipsec", "ppp", "gif", "stf", "tun"]
        .iter()
        .any(|p| {
            name.strip_prefix(p)
                .is_some_and(|rest| rest.bytes().all(|c| c.is_ascii_digit()))
        })
}

/// What SystemConfiguration says a BSD interface is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScType {
    Wifi,
    Ethernet,
    Other,
}

/// Classifies an interface: loopback and tunnels by name, WiFi and Ethernet by
/// SystemConfiguration's interface type, anything else is `Other`.
pub fn classify(name: &str, is_loopback: bool, sc: &HashMap<String, ScType>) -> LinkKind {
    if is_loopback || name.starts_with("lo") && name[2..].bytes().all(|c| c.is_ascii_digit()) {
        return LinkKind::Loopback;
    }
    if is_tunnel_name(name) {
        return LinkKind::Tunnel;
    }
    match sc.get(name) {
        Some(ScType::Wifi) => LinkKind::Wifi,
        Some(ScType::Ethernet) => LinkKind::Ethernet,
        _ => LinkKind::Other,
    }
}

// ---------------------------------------------------------------------------
// The system view
// ---------------------------------------------------------------------------

/// How long the SystemConfiguration interface list is reused. It changes when
/// hardware is added, not with the link, so it follows the 30 s address cadence.
const SC_REFRESH_MS: u64 = 30_000;

#[derive(Default)]
pub struct MacNetworkSystem {
    sc: HashMap<String, ScType>,
    sc_read_at: Option<std::time::Instant>,
}

impl NetworkSystem for MacNetworkSystem {
    fn read(&mut self) -> Option<SystemView> {
        let stale = self
            .sc_read_at
            .is_none_or(|t| t.elapsed().as_millis() as u64 >= SC_REFRESH_MS);
        if stale {
            self.sc = imp::system_configuration_types();
            self.sc_read_at = Some(std::time::Instant::now());
        }
        let links = imp::link_interfaces()?;
        let media = imp::MediaQuery::open();
        let mut interfaces: Vec<InterfaceInfo> = links
            .into_iter()
            .map(|l| {
                let kind = classify(&l.name, l.loopback, &self.sc);
                // A carrier or association, asked only of up WiFi and Ethernet
                // ports: an unplugged port is still up and running on macOS.
                let link_active = l.up_running
                    && matches!(kind, LinkKind::Wifi | LinkKind::Ethernet)
                    && media.as_ref().is_some_and(|m| m.active(&l.name));
                InterfaceInfo {
                    link_active,
                    station: if kind == LinkKind::Wifi { l.mac } else { None },
                    // Only an interface that is up has counters worth a syscall.
                    counters: if l.up_running {
                        imp::sysctl_buffer(&[imp::CTL_NET, 18, 0, 2, l.index as i32, 1])
                            .and_then(|b| parse_ifmib(&b))
                    } else {
                        None
                    },
                    name: l.name,
                    index: l.index,
                    kind,
                    up_running: l.up_running,
                }
            })
            .collect();
        interfaces.sort_by_key(|i| i.index);
        let mut default_routes = Vec::new();
        for family in [i32::from(AF_INET), i32::from(AF_INET6)] {
            let Some(buf) = imp::sysctl_buffer(&[imp::CTL_NET, imp::PF_ROUTE, 0, family, 1, 0])
            else {
                continue;
            };
            for (index, scoped) in parse_default_routes(&buf) {
                if let Some(i) = interfaces.iter().find(|i| i.index == index) {
                    default_routes.push(DefaultRoute {
                        interface: i.name.clone(),
                        scoped,
                    });
                }
            }
        }
        Some(SystemView {
            interfaces,
            default_routes,
        })
    }

    fn invalidate(&mut self) {
        self.sc_read_at = None;
    }
}

/// One `AF_LINK` entry of `getifaddrs`.
struct LinkInterface {
    name: String,
    index: u32,
    up_running: bool,
    loopback: bool,
    mac: Option<[u8; 6]>,
}

#[cfg(target_os = "macos")]
mod imp {
    use super::{parse_link_address, LinkInterface, ScType, AF_LINK};
    use crate::cf::{self, Cf};
    use std::collections::HashMap;
    use std::ffi::{c_char, c_int, c_void, CStr};

    pub const CTL_NET: c_int = 4;
    pub const PF_ROUTE: c_int = 17;
    const IFF_UP: u32 = 0x1;
    const IFF_LOOPBACK: u32 = 0x8;
    const IFF_RUNNING: u32 = 0x40;

    #[repr(C)]
    struct IfAddrs {
        ifa_next: *mut IfAddrs,
        ifa_name: *mut c_char,
        ifa_flags: u32,
        ifa_addr: *mut u8,
        ifa_netmask: *mut u8,
        ifa_dstaddr: *mut u8,
        ifa_data: *mut c_void,
    }

    extern "C" {
        fn getifaddrs(ifap: *mut *mut IfAddrs) -> c_int;
        fn freeifaddrs(ifa: *mut IfAddrs);
        fn sysctl(
            name: *mut c_int,
            namelen: u32,
            oldp: *mut c_void,
            oldlenp: *mut usize,
            newp: *mut c_void,
            newlen: usize,
        ) -> c_int;
    }

    #[link(name = "SystemConfiguration", kind = "framework")]
    extern "C" {
        fn SCNetworkInterfaceCopyAll() -> *const c_void;
        fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
        fn ioctl(fd: c_int, request: std::ffi::c_ulong, ...) -> c_int;
        fn close(fd: c_int) -> c_int;
        fn SCNetworkInterfaceGetBSDName(interface: *const c_void) -> *const c_void;
        fn SCNetworkInterfaceGetInterfaceType(interface: *const c_void) -> *const c_void;
    }

    /// Frees the list from `getifaddrs`.
    struct IfList(*mut IfAddrs);
    impl Drop for IfList {
        fn drop(&mut self) {
            // SAFETY: the pointer came from a successful getifaddrs.
            unsafe { freeifaddrs(self.0) };
        }
    }

    pub fn link_interfaces() -> Option<Vec<LinkInterface>> {
        let mut head: *mut IfAddrs = std::ptr::null_mut();
        // SAFETY: a valid out pointer.
        if unsafe { getifaddrs(&mut head) } != 0 || head.is_null() {
            return None;
        }
        let list = IfList(head);
        let mut out = Vec::new();
        let mut cursor = list.0;
        while !cursor.is_null() {
            // SAFETY: a node of the live list, valid until `list` drops.
            let entry = unsafe { &*cursor };
            cursor = entry.ifa_next;
            if entry.ifa_addr.is_null() || entry.ifa_name.is_null() {
                continue;
            }
            // SAFETY: every sockaddr starts with sa_len; the kernel filled
            // sa_len bytes.
            let sa = unsafe {
                let len = usize::from(*entry.ifa_addr);
                std::slice::from_raw_parts(entry.ifa_addr, len.max(2))
            };
            if sa[1] != AF_LINK || sa.len() < 8 {
                continue;
            }
            // SAFETY: NUL-terminated by the kernel.
            let name = unsafe { CStr::from_ptr(entry.ifa_name) }
                .to_string_lossy()
                .into_owned();
            let index = u32::from(u16::from_ne_bytes([sa[2], sa[3]]));
            out.push(LinkInterface {
                name,
                index,
                up_running: entry.ifa_flags & (IFF_UP | IFF_RUNNING) == (IFF_UP | IFF_RUNNING),
                loopback: entry.ifa_flags & IFF_LOOPBACK != 0,
                mac: parse_link_address(sa),
            });
        }
        Some(out)
    }

    /// Runs a sysctl that returns a buffer, growing until it fits.
    /// `struct ifmediareq` from `<net/if.h>`, declared there under
    /// `#pragma pack(4)` (44 bytes, not 48); the size is part of the ioctl
    /// number, so the layout must match exactly. Only the status is read.
    #[repr(C, packed(4))]
    struct IfMediaReq {
        name: [u8; 16],
        current: c_int,
        mask: c_int,
        status: c_int,
        active: c_int,
        count: c_int,
        list: *mut c_int,
    }
    const _: () = assert!(std::mem::size_of::<IfMediaReq>() == 44);
    const IFM_AVALID: c_int = 0x1;
    const IFM_ACTIVE: c_int = 0x2;
    // _IOWR('i', 56, struct ifmediareq)
    const SIOCGIFMEDIA: std::ffi::c_ulong = 0xC000_0000
        | (((std::mem::size_of::<IfMediaReq>() & 0x1fff) as std::ffi::c_ulong) << 16)
        | ((b'i' as std::ffi::c_ulong) << 8)
        | 56;

    /// One datagram socket for `SIOCGIFMEDIA` queries, closed on drop.
    pub struct MediaQuery(c_int);

    impl MediaQuery {
        pub fn open() -> Option<Self> {
            // SAFETY: plain socket creation; checked below.
            let fd = unsafe {
                socket(2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0)
            };
            (fd >= 0).then_some(Self(fd))
        }

        /// Whether `name` has an active link (carrier, or WiFi association),
        /// as `ifconfig` reports `status: active`. An interface without media
        /// reporting is not active.
        pub fn active(&self, name: &str) -> bool {
            let mut request = IfMediaReq {
                name: [0; 16],
                current: 0,
                mask: 0,
                status: 0,
                active: 0,
                count: 0,
                list: std::ptr::null_mut(),
            };
            let bytes = name.as_bytes();
            if bytes.len() >= request.name.len() {
                return false;
            }
            request.name[..bytes.len()].copy_from_slice(bytes);
            // SAFETY: a valid socket and a properly sized, NUL-terminated request
            // with no media list to fill.
            let ok = unsafe { ioctl(self.0, SIOCGIFMEDIA, &mut request as *mut IfMediaReq) } == 0;
            let status = request.status;
            ok && status & IFM_AVALID != 0 && status & IFM_ACTIVE != 0
        }
    }

    impl Drop for MediaQuery {
        fn drop(&mut self) {
            // SAFETY: the descriptor came from socket() and is closed once.
            unsafe { close(self.0) };
        }
    }

    pub fn sysctl_buffer(mib: &[c_int]) -> Option<Vec<u8>> {
        let mut mib = mib.to_vec();
        for _ in 0..4 {
            let mut len = 0usize;
            // SAFETY: a size query with a null buffer.
            if unsafe {
                sysctl(
                    mib.as_mut_ptr(),
                    mib.len() as u32,
                    std::ptr::null_mut(),
                    &mut len,
                    std::ptr::null_mut(),
                    0,
                )
            } != 0
            {
                return None;
            }
            len += len / 8 + 4096; // the table can grow between the calls
            let mut buf = vec![0u8; len];
            // SAFETY: `buf` has `len` writable bytes.
            let status = unsafe {
                sysctl(
                    mib.as_mut_ptr(),
                    mib.len() as u32,
                    buf.as_mut_ptr().cast(),
                    &mut len,
                    std::ptr::null_mut(),
                    0,
                )
            };
            if status == 0 {
                buf.truncate(len);
                return Some(buf);
            }
        }
        None
    }

    /// BSD name to interface type for every interface SystemConfiguration lists.
    pub fn system_configuration_types() -> HashMap<String, ScType> {
        let mut out = HashMap::new();
        // SAFETY: a +1 CFArray or null, released by `Cf`.
        let Some(all) = (unsafe { Cf::owned(SCNetworkInterfaceCopyAll()) }) else {
            return out;
        };
        for interface in cf::array_values(all.get()) {
            // SAFETY: elements are borrowed from `all`, alive until it drops;
            // the names are borrowed from each element (Get rule).
            let (name, kind) = unsafe {
                (
                    cf::string_value::<128>(SCNetworkInterfaceGetBSDName(interface)),
                    cf::string_value::<128>(SCNetworkInterfaceGetInterfaceType(interface)),
                )
            };
            if let (Some(name), Some(kind)) = (name, kind) {
                let kind = match kind.as_str() {
                    "IEEE80211" => ScType::Wifi,
                    "Ethernet" => ScType::Ethernet,
                    _ => ScType::Other,
                };
                out.insert(name, kind);
            }
        }
        out
    }
}

#[cfg(not(target_os = "macos"))]
mod imp {
    use super::{LinkInterface, ScType};
    use std::collections::HashMap;
    use std::ffi::c_int;

    pub const CTL_NET: c_int = 4;
    pub const PF_ROUTE: c_int = 17;

    pub fn link_interfaces() -> Option<Vec<LinkInterface>> {
        None
    }
    pub fn sysctl_buffer(_mib: &[c_int]) -> Option<Vec<u8>> {
        None
    }
    pub fn system_configuration_types() -> HashMap<String, ScType> {
        HashMap::new()
    }
    pub struct MediaQuery;
    impl MediaQuery {
        pub fn open() -> Option<Self> {
            None
        }
        pub fn active(&self, _name: &str) -> bool {
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn put16(b: &mut [u8], at: usize, v: u16) {
        b[at..at + 2].copy_from_slice(&v.to_ne_bytes());
    }
    fn put32(b: &mut [u8], at: usize, v: i32) {
        b[at..at + 4].copy_from_slice(&v.to_ne_bytes());
    }
    fn put64(b: &mut [u8], at: usize, v: u64) {
        b[at..at + 8].copy_from_slice(&v.to_ne_bytes());
    }

    fn ifmib(rx: u64, tx: u64) -> Vec<u8> {
        let mut m = vec![0u8; IFMIB_LEN];
        put64(&mut m, IFMIB_DATA + DATA64_IBYTES, rx);
        put64(&mut m, IFMIB_DATA + DATA64_OBYTES, tx);
        m
    }

    #[test]
    fn ifmib_yields_full_64_bit_byte_counters() {
        // Above 2^32: a 32-bit read would have truncated these.
        let c = parse_ifmib(&ifmib(186_782_805_861, 5_616_479_565)).unwrap();
        assert_eq!((c.rx_bytes, c.tx_bytes), (186_782_805_861, 5_616_479_565));
        assert_eq!(parse_ifmib(&ifmib(u64::MAX, 1)).unwrap().rx_bytes, u64::MAX);
        assert_eq!(parse_ifmib(&ifmib(1, 2)[..IFMIB_LEN - 1]), None);
        assert_eq!(parse_ifmib(&[]), None);
    }

    /// A route message with a destination and optionally a netmask.
    fn route_msg(index: u16, flags: i32, dst: &[u8], mask: Option<&[u8]>) -> Vec<u8> {
        let pad = |b: &[u8]| {
            let mut v = b.to_vec();
            v.resize(if b[0] == 0 { 4 } else { (b.len() + 3) & !3 }, 0);
            v
        };
        let mut body = pad(dst);
        body.extend(pad(&[0u8; 8])); // a gateway sockaddr is always present
        let mut addrs = RTA_DST | 0x2;
        if let Some(m) = mask {
            body.extend(pad(m));
            addrs |= RTA_NETMASK;
        }
        let mut m = vec![0u8; RT_MSGHDR_LEN];
        m.extend(body);
        let len = m.len() as u16;
        put16(&mut m, 0, len);
        m[3] = 4;
        put16(&mut m, 4, index);
        put32(&mut m, 8, flags);
        put32(&mut m, 12, addrs);
        m
    }

    fn v4(a: [u8; 4]) -> Vec<u8> {
        let mut s = vec![16, AF_INET, 0, 0];
        s.extend(a);
        s.extend([0u8; 8]);
        s
    }

    #[test]
    fn default_routes_are_found_with_their_scope() {
        let mut buf = Vec::new();
        buf.extend(route_msg(14, RTF_UP | 2, &v4([0; 4]), Some(&[0, 0, 0, 0])));
        buf.extend(route_msg(
            14,
            RTF_UP,
            &v4([192, 168, 8, 0]),
            Some(&v4([255, 255, 255, 0])),
        ));
        buf.extend(route_msg(9, RTF_UP | 2 | RTF_IFSCOPE, &v4([0; 4]), None));
        buf.extend(route_msg(5, 2, &v4([0; 4]), None)); // not up
        buf.extend(route_msg(6, RTF_UP | RTF_REJECT, &v4([0; 4]), None));
        buf.extend(route_msg(7, RTF_UP | RTF_BLACKHOLE, &v4([0; 4]), None));
        assert_eq!(parse_default_routes(&buf), vec![(14, false), (9, true)]);
    }

    #[test]
    fn an_ipv6_default_route_and_a_short_mask_are_understood() {
        let mut dst = vec![28, AF_INET6, 0, 0, 0, 0, 0, 0];
        dst.extend([0u8; 16]);
        dst.extend([0u8; 4]);
        let buf = route_msg(20, RTF_UP | 2, &dst, Some(&[0]));
        assert_eq!(parse_default_routes(&buf), vec![(20, false)]);
        let mut named = dst.clone();
        named[8] = 0xfe; // fe80::/..: not a default route
        assert!(parse_default_routes(&route_msg(20, RTF_UP, &named, None)).is_empty());
    }

    #[test]
    fn a_truncated_or_garbage_route_dump_yields_nothing_and_never_panics() {
        let good = route_msg(14, RTF_UP | 2, &v4([0; 4]), None);
        for cut in 0..good.len() {
            let _ = parse_default_routes(&good[..cut]);
        }
        let mut lying = good.clone();
        put32(&mut lying, 12, 0xff); // claims every address, has few
        assert!(parse_default_routes(&lying).is_empty());
        assert!(parse_default_routes(&[0xff; 64]).is_empty());
    }

    fn link_sockaddr(name: &str, mac: &[u8]) -> Vec<u8> {
        let mut s = vec![0u8; 8];
        s[1] = AF_LINK;
        s[5] = name.len() as u8;
        s[6] = mac.len() as u8;
        s.extend(name.as_bytes());
        s.extend(mac);
        s[0] = s.len() as u8;
        s
    }

    #[test]
    fn a_link_address_is_read_past_the_interface_name() {
        let sa = link_sockaddr("en0", &[0xf2, 0xa1, 0xb3, 0xc4, 0xd5, 0xe6]);
        assert_eq!(
            parse_link_address(&sa),
            Some([0xf2, 0xa1, 0xb3, 0xc4, 0xd5, 0xe6])
        );
        assert_eq!(parse_link_address(&link_sockaddr("en0", &[0; 6])), None);
        assert_eq!(parse_link_address(&link_sockaddr("gif0", &[])), None);
        assert_eq!(
            parse_link_address(&link_sockaddr("x", &[1, 2, 3, 4, 5])),
            None
        );
        let mut truncated = sa.clone();
        truncated.truncate(12);
        assert_eq!(parse_link_address(&truncated), None);
        assert_eq!(parse_link_address(&[]), None);
    }

    #[test]
    fn interfaces_are_classified_by_name_and_system_configuration() {
        let sc: HashMap<String, ScType> = [
            ("en0".to_string(), ScType::Wifi),
            ("en5".to_string(), ScType::Ethernet),
            ("bridge0".to_string(), ScType::Other),
        ]
        .into();
        assert_eq!(classify("en0", false, &sc), LinkKind::Wifi);
        assert_eq!(classify("en5", false, &sc), LinkKind::Ethernet);
        assert_eq!(classify("bridge0", false, &sc), LinkKind::Other);
        assert_eq!(classify("utun4", false, &sc), LinkKind::Tunnel);
        assert_eq!(classify("ipsec0", false, &sc), LinkKind::Tunnel);
        assert_eq!(classify("lo0", true, &sc), LinkKind::Loopback);
        assert_eq!(classify("awdl0", false, &sc), LinkKind::Other);
        // A name that merely starts like a tunnel is not one.
        assert_eq!(classify("utunnel", false, &sc), LinkKind::Other);
    }

    /// Prints what this Mac reports right now (no addresses, SSIDs or BSSIDs):
    /// `cargo test -p platform-macos observe_this_mac -- --ignored --nocapture`.
    #[test]
    #[ignore = "reads the real network; run by hand"]
    fn observe_this_mac() {
        use metrics::wifi_context::FixtureProvider;
        use metrics::Collector;
        let mut view = MacNetworkSystem::default();
        let v = metrics::network::NetworkSystem::read(&mut view).expect("system view");
        for i in &v.interfaces {
            println!(
                "if {} idx {} {:?} up_running {} link_active {} station {} counters {:?}",
                i.name,
                i.index,
                i.kind,
                i.up_running,
                i.link_active,
                if i.station.is_some() {
                    "present"
                } else {
                    "absent"
                },
                i.counters
            );
        }
        println!("default routes: {:?}", v.default_routes);
        println!(
            "primary: {:?}",
            metrics::network::select_primary_route(&v).map(|p| (p.interface, p.kind, p.via_tunnel))
        );
        let mut c = collector(Box::new(FixtureProvider::absent()));
        for second in 0..3u64 {
            let r = c.collect(second * 1000).value.unwrap();
            println!(
                "t={}s traffic {:?} kind {:?} tunnel {} rate {:?} | context {:?} coverage {:?} station {}",
                second,
                r.traffic.availability,
                r.traffic.route_kind,
                r.traffic.via_tunnel,
                r.traffic.rate,
                r.wifi.availability,
                r.wifi.coverage,
                if r.wifi.station.is_some() { "present" } else { "absent" }
            );
            std::thread::sleep(std::time::Duration::from_millis(1000));
        }
    }
}
