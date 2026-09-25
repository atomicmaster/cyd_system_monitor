// SPDX-License-Identifier: GPL-3.0-only

//! Host-side codec home: framing, version negotiation, and encode/decode
//! against `protocol-generated` types (see `protocol/`). Empty at M0; C01
//! defines the first real USB message and this crate's decoder.

pub use protocol_generated::Smoke;

/// Bounded versioned-CBOR-over-USB core shared semantically with the
/// firmware codec. Frames are `u16-be length | CBOR | CRC32-be`.
pub const MAX_FRAME_BYTES: usize = 2048;
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Truncated,
    TooLarge,
    BadCrc,
    Malformed,
    UnsupportedMajor,
    WrongSession,
    StaleSequence,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageType {
    Hello = 1,
    HelloReply = 2,
    Pair = 3,
    Snapshot = 4,
    Command = 5,
    CommandReply = 6,
    Error = 7,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Availability {
    Available = 0,
    Unsupported = 1,
    Absent = 2,
    TemporarilyUnavailable = 3,
    Error = 4,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Freshness {
    Fresh = 0,
    Stale = 1,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Envelope {
    pub major: u8,
    pub minor: u8,
    pub kind: MessageType,
    pub session: [u8; 16],
    pub sequence: u32,
    pub utc_ms: Option<u64>,
    pub utc_uncertainty_ms: Option<u32>,
    /// The Host's local offset from UTC in minutes east (wire value biased by 1440).
    pub local_offset_minutes: Option<i16>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RamSnapshot {
    pub availability: Availability,
    pub used_bytes: u64,
    pub total_bytes: u64,
    pub source_age_ms: u32,
    pub freshness: Freshness,
}
/// Root storage group (TB04): capacity and disk I/O, independent availability
/// and source ages, no freshness field (the device derives it from the ages).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StorageSnapshot {
    pub capacity_availability: Availability,
    pub total_bytes: u64,
    pub available_bytes: u64,
    pub capacity_age_ms: u32,
    pub io_availability: Availability,
    pub read_bytes_per_s: u64,
    pub write_bytes_per_s: u64,
    pub io_age_ms: u32,
}
/// Battery group (TB06): charge/state and condition, independent availability
/// and source ages, no freshness field (the device derives it from the ages).
/// A half that is not available carries 0 (or the unknown sentinel) values.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BatterySnapshot {
    pub charge_availability: Availability,
    pub charge_per_mille: u16,
    /// 0 unknown, 1 charging, 2 discharging, 3 charged, 4 on AC not charging.
    pub state: u8,
    /// Minutes to empty or full; `BATTERY_UNKNOWN_U16` when not reported.
    pub time_remaining_min: u16,
    pub charge_age_ms: u32,
    pub condition_availability: Availability,
    pub max_capacity_per_mille: u16,
    /// 0 unknown, 1 normal, 2 service recommended, 3 poor/replace.
    pub condition: u8,
    /// Charge cycles; `BATTERY_UNKNOWN_U16` when not reported.
    pub cycle_count: u16,
    pub condition_age_ms: u32,
}
pub const BATTERY_UNKNOWN_U16: u16 = 0xFFFF;
/// Agents group (TB07): exact same-user process presence for the three
/// supported agents in fixed slot order [codex, claude, grok]. Each state is
/// 0 not running, 1 running with activity unknown, 2 busy, 3 idle (2 and 3
/// need an activity adapter). Not Available means all states are 0.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AgentsSnapshot {
    pub availability: Availability,
    pub source_age_ms: u32,
    pub state: [u8; 3],
}
/// The kind of the primary-route interface (Network group, key 2).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RouteKind {
    Unknown = 0,
    Wifi = 1,
    Ethernet = 2,
    /// Another link, e.g. a Thunderbolt bridge or cellular.
    Other = 3,
    /// No primary route: offline.
    None = 4,
}
/// Association Context Availability (CONTEXT.md): why SSID/BSSID context is or
/// is not usable. Independent of the metric `Availability`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AssociationContext {
    Available = 0,
    PermissionNotRequested = 1,
    Denied = 2,
    Restricted = 3,
    AuthorizedUnavailable = 4,
    Unsupported = 5,
    Error = 6,
}
/// Host WiFi Coverage: observable (2.4 GHz, not necessarily captured now),
/// outside supported coverage (5/6 GHz) or unknown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WifiCoverage {
    Unknown = 0,
    Observable = 1,
    OutsideSupported = 2,
}
pub const MAX_SSID_BYTES: usize = 32;
/// Network group (TB05): primary-route traffic and Host WiFi context, two
/// halves with independent availability and source ages. A traffic half that
/// is not Available has zero rates; SSID/BSSID are present only when
/// `context_availability` is Available (`CORE_PROTOCOL.md`, group 9).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetworkSnapshot {
    pub traffic_availability: Availability,
    pub traffic_age_ms: u32,
    pub route_kind: RouteKind,
    /// The rates are the tunnel's own; never summed with the underlying link.
    pub via_tunnel: bool,
    pub rx_bytes_per_s: u64,
    pub tx_bytes_per_s: u64,
    pub context_availability: AssociationContext,
    pub context_age_ms: u32,
    pub coverage: WifiCoverage,
    /// 0 unknown; 0 whenever coverage is unknown.
    pub channel: u8,
    /// Host Station Identity; independent of Location permission.
    pub station: Option<[u8; 6]>,
    /// 0-32 raw octets.
    pub ssid: Vec<u8>,
    pub bssid: Option<[u8; 6]>,
}
impl NetworkSnapshot {
    /// The rules shared with the C++ `ValidNetwork`: an encoder refuses and a
    /// decoder rejects what breaks them.
    pub fn is_valid(&self) -> bool {
        if self.via_tunnel && matches!(self.route_kind, RouteKind::Unknown | RouteKind::None) {
            return false;
        }
        if self.traffic_availability != Availability::Available
            && (self.rx_bytes_per_s != 0 || self.tx_bytes_per_s != 0)
        {
            return false;
        }
        if self.coverage == WifiCoverage::Unknown && self.channel != 0 {
            return false;
        }
        if self.ssid.len() > MAX_SSID_BYTES {
            return false;
        }
        (self.ssid.is_empty() && self.bssid.is_none())
            || self.context_availability == AssociationContext::Available
    }
}
/// A complete snapshot: CPU plus each optional metric group. A new group is
/// one field here.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Snapshot {
    pub cpu: CpuSnapshot,
    pub ram: Option<RamSnapshot>,
    pub storage: Option<StorageSnapshot>,
    pub battery: Option<BatterySnapshot>,
    pub agents: Option<AgentsSnapshot>,
    pub network: Option<NetworkSnapshot>,
}
impl Snapshot {
    /// CPU alone, no optional group; add groups with struct update syntax.
    pub fn new(cpu: CpuSnapshot) -> Self {
        Self {
            cpu,
            ram: None,
            storage: None,
            battery: None,
            agents: None,
            network: None,
        }
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CpuSnapshot {
    pub availability: Availability,
    pub utilization_per_mille: u16,
    pub source_age_ms: u32,
    pub freshness: Freshness,
    pub configuration_revision: u32,
}

/// Stateful negotiated-session sequence gate, matching the firmware codec.
#[derive(Debug, Default)]
pub struct SessionReceiver {
    session: Option<[u8; 16]>,
    last_sequence: u32,
}

impl SessionReceiver {
    pub fn accept(&mut self, envelope: &Envelope) -> Result<(), Error> {
        match self.session {
            None => self.reset(envelope.session),
            Some(session) if session != envelope.session => return Err(Error::WrongSession),
            _ => {}
        }
        if envelope.sequence <= self.last_sequence {
            return Err(Error::StaleSequence);
        }
        self.last_sequence = envelope.sequence;
        Ok(())
    }

    pub fn reset(&mut self, session: [u8; 16]) {
        self.session = Some(session);
        self.last_sequence = 0;
    }
}

/// Byte-stream framer matching the firmware `FrameStream`. A frame with an
/// invalid length or CRC cannot mark its own end, so one byte is discarded and
/// the scan resumes; a frame with a valid length and CRC is consumed whole and
/// its decode result (possibly an error) is returned.
#[derive(Debug, Default)]
pub struct FrameStream {
    buffer: Vec<u8>,
    dropped_bytes: usize,
}

impl FrameStream {
    pub fn push(&mut self, bytes: &[u8]) {
        self.buffer.extend_from_slice(bytes);
    }
    pub fn dropped_bytes(&self) -> usize {
        self.dropped_bytes
    }
    pub fn buffered_bytes(&self) -> usize {
        self.buffer.len()
    }
    /// Gives up on a stalled partial frame: discards one byte so the caller
    /// can rescan with `next_frame`. For use after an idle gap in the stream.
    pub fn drop_leading_byte(&mut self) {
        if !self.buffer.is_empty() {
            self.buffer.remove(0);
            self.dropped_bytes += 1;
        }
    }
    /// Returns the next complete frame's bytes, or `None` if more input is
    /// needed. Decode the returned frame with `decode_cpu`/`decode_control`.
    pub fn next_frame(&mut self) -> Option<Vec<u8>> {
        while self.buffer.len() >= 2 {
            let n = u16::from_be_bytes([self.buffer[0], self.buffer[1]]) as usize;
            if !(5..=MAX_FRAME_BYTES - 2).contains(&n) {
                self.buffer.remove(0);
                self.dropped_bytes += 1;
                continue;
            }
            if self.buffer.len() < n + 2 {
                return None;
            }
            let end = n + 2;
            let crc = u32::from_be_bytes(self.buffer[end - 4..end].try_into().unwrap());
            if crc32(&self.buffer[2..end - 4]) != crc {
                self.buffer.remove(0);
                self.dropped_bytes += 1;
                continue;
            }
            return Some(self.buffer.drain(..end).collect());
        }
        None
    }
}

/// The capability/revision exchange used by both `Hello` messages.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HelloBody {
    pub supported_major: u8,
    pub supported_minor: u8,
    pub capabilities: u32,
    pub configuration_revision: u32,
    pub utc_ms: Option<u64>,
    pub utc_uncertainty_ms: Option<u32>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairBody {
    pub pairing_identity: [u8; 16],
    pub intended_peer_identity: [u8; 16],
    pub matching_code: u32,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CommandOperation {
    Unpair = 1,
    CalibrationReset = 2,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommandBody {
    pub command_identity: u32,
    pub operation: CommandOperation,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CommandResult {
    Applied = 0,
    AlreadyApplied = 1,
    Rejected = 2,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommandReplyBody {
    pub command_identity: u32,
    pub operation: CommandOperation,
    pub result: CommandResult,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ControlBody {
    Hello(HelloBody),
    HelloReply(HelloBody),
    Pair(PairBody),
    Command(CommandBody),
    CommandReply(CommandReplyBody),
    /// Opaque, bounded diagnostic bytes; never applied as metrics or config.
    Error(Vec<u8>),
}

fn uint(out: &mut Vec<u8>, major: u8, value: u64) {
    if value < 24 {
        out.push((major << 5) | value as u8)
    } else if value <= 255 {
        out.extend([(major << 5) | 24, value as u8])
    } else if value <= 65535 {
        out.extend([(major << 5) | 25, (value >> 8) as u8, value as u8])
    } else if value <= u32::MAX as u64 {
        out.push((major << 5) | 26);
        out.extend((value as u32).to_be_bytes())
    } else {
        out.push((major << 5) | 27);
        out.extend(value.to_be_bytes())
    }
}
fn read_uint(input: &[u8], p: &mut usize, expected: u8) -> Option<u64> {
    let first = *input.get(*p)?;
    if first >> 5 != expected {
        return None;
    }
    *p += 1;
    let a = first & 31;
    let n = match a {
        0..=23 => return Some(a as u64),
        24 => 1,
        25 => 2,
        26 => 4,
        27 => 8,
        _ => return None,
    };
    if *p + n > input.len() {
        return None;
    }
    let mut v = 0;
    for _ in 0..n {
        v = (v << 8) | input[*p] as u64;
        *p += 1
    }
    if (a == 24 && v < 24)
        || (a == 25 && v <= 255)
        || (a == 26 && v <= 65535)
        || (a == 27 && v <= u32::MAX as u64)
    {
        return None;
    }
    Some(v)
}
pub fn crc32(bytes: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for b in bytes {
        c ^= *b as u32;
        for _ in 0..8 {
            c = (c >> 1) ^ (0xedb8_8320 & (0u32.wrapping_sub(c & 1)));
        }
    }
    !c
}
pub fn encode_cpu(envelope: &Envelope, cpu: &CpuSnapshot) -> Result<Vec<u8>, Error> {
    encode_snapshot(envelope, cpu, None)
}
/// Encodes a complete snapshot: CPU at keys 0-4 and the RAM group at key 5
/// when given.
pub fn encode_snapshot(
    envelope: &Envelope,
    cpu: &CpuSnapshot,
    ram: Option<&RamSnapshot>,
) -> Result<Vec<u8>, Error> {
    encode_groups(
        envelope,
        &Snapshot {
            ram: ram.cloned(),
            ..Snapshot::new(cpu.clone())
        },
    )
}
/// Encodes a complete snapshot with each optional group it carries (RAM at
/// key 5, Storage at key 6, Battery at key 7, Agents at key 8, Network at key 9).
pub fn encode_groups(envelope: &Envelope, snapshot: &Snapshot) -> Result<Vec<u8>, Error> {
    let cpu = &snapshot.cpu;
    let ram = snapshot.ram.as_ref();
    let storage = snapshot.storage.as_ref();
    let battery = snapshot.battery.as_ref();
    let agents = snapshot.agents.as_ref();
    let network = snapshot.network.as_ref();
    if ram.is_some_and(|r| r.used_bytes > r.total_bytes)
        || storage.is_some_and(|s| s.available_bytes > s.total_bytes)
        || battery.is_some_and(|b| {
            b.charge_per_mille > 1000
                || b.max_capacity_per_mille > 1000
                || b.state > 4
                || b.condition > 3
        })
        || agents.is_some_and(|a| a.state.iter().any(|&s| s > 3))
        || network.is_some_and(|n| !n.is_valid())
    {
        return Err(Error::Malformed);
    }
    let mut c = vec![
        0xa6 + envelope.utc_ms.is_some() as u8
            + envelope.utc_uncertainty_ms.is_some() as u8
            + envelope.local_offset_minutes.is_some() as u8,
    ];
    for (k, v) in [
        (0, envelope.major as u64),
        (1, envelope.minor as u64),
        (2, envelope.kind as u64),
    ] {
        uint(&mut c, 0, k);
        uint(&mut c, 0, v)
    }
    uint(&mut c, 0, 3);
    uint(&mut c, 2, 16);
    c.extend(envelope.session);
    uint(&mut c, 0, 4);
    uint(&mut c, 0, envelope.sequence as u64);
    uint(&mut c, 0, 5);
    c.push(
        0xa5 + ram.is_some() as u8
            + storage.is_some() as u8
            + battery.is_some() as u8
            + agents.is_some() as u8
            + network.is_some() as u8,
    );
    for (k, v) in [
        (0, cpu.availability as u64),
        (1, cpu.utilization_per_mille as u64),
        (2, cpu.source_age_ms as u64),
        (3, cpu.freshness as u64),
        (4, cpu.configuration_revision as u64),
    ] {
        uint(&mut c, 0, k);
        uint(&mut c, 0, v)
    }
    if let Some(ram) = ram {
        uint(&mut c, 0, 5);
        c.push(0xa5);
        for (k, v) in [
            (0, ram.availability as u64),
            (1, ram.used_bytes),
            (2, ram.total_bytes),
            (3, ram.source_age_ms as u64),
            (4, ram.freshness as u64),
        ] {
            uint(&mut c, 0, k);
            uint(&mut c, 0, v)
        }
    }
    if let Some(storage) = storage {
        uint(&mut c, 0, 6);
        c.push(0xa8);
        for (k, v) in [
            (0, storage.capacity_availability as u64),
            (1, storage.total_bytes),
            (2, storage.available_bytes),
            (3, storage.capacity_age_ms as u64),
            (4, storage.io_availability as u64),
            (5, storage.read_bytes_per_s),
            (6, storage.write_bytes_per_s),
            (7, storage.io_age_ms as u64),
        ] {
            uint(&mut c, 0, k);
            uint(&mut c, 0, v)
        }
    }
    if let Some(battery) = battery {
        uint(&mut c, 0, 7);
        c.push(0xaa);
        for (k, v) in [
            (0, battery.charge_availability as u64),
            (1, battery.charge_per_mille as u64),
            (2, battery.state as u64),
            (3, battery.time_remaining_min as u64),
            (4, battery.charge_age_ms as u64),
            (5, battery.condition_availability as u64),
            (6, battery.max_capacity_per_mille as u64),
            (7, battery.condition as u64),
            (8, battery.cycle_count as u64),
            (9, battery.condition_age_ms as u64),
        ] {
            uint(&mut c, 0, k);
            uint(&mut c, 0, v)
        }
    }
    if let Some(agents) = agents {
        uint(&mut c, 0, 8);
        c.push(0xa5);
        for (k, v) in [
            (0, agents.availability as u64),
            (1, agents.source_age_ms as u64),
            (2, agents.state[0] as u64),
            (3, agents.state[1] as u64),
            (4, agents.state[2] as u64),
        ] {
            uint(&mut c, 0, k);
            uint(&mut c, 0, v)
        }
    }
    if let Some(network) = network {
        uint(&mut c, 0, 9);
        c.push(0xad);
        for (k, v) in [
            (0, network.traffic_availability as u64),
            (1, network.traffic_age_ms as u64),
            (2, network.route_kind as u64),
            (3, network.via_tunnel as u64),
            (4, network.rx_bytes_per_s),
            (5, network.tx_bytes_per_s),
            (6, network.context_availability as u64),
            (7, network.context_age_ms as u64),
            (8, network.coverage as u64),
            (9, network.channel as u64),
        ] {
            uint(&mut c, 0, k);
            uint(&mut c, 0, v)
        }
        let station: &[u8] = network.station.as_ref().map_or(&[], |m| m);
        let bssid: &[u8] = network.bssid.as_ref().map_or(&[], |m| m);
        for (k, bytes) in [(10, station), (11, &network.ssid[..]), (12, bssid)] {
            uint(&mut c, 0, k);
            uint(&mut c, 2, bytes.len() as u64);
            c.extend(bytes);
        }
    }
    if let Some(utc_ms) = envelope.utc_ms {
        uint(&mut c, 0, 6);
        uint(&mut c, 0, utc_ms);
    }
    if let Some(utc_uncertainty_ms) = envelope.utc_uncertainty_ms {
        uint(&mut c, 0, 7);
        uint(&mut c, 0, utc_uncertainty_ms as u64);
    }
    if let Some(offset) = envelope.local_offset_minutes {
        if !(-1440..=1440).contains(&offset) {
            return Err(Error::Malformed);
        }
        uint(&mut c, 0, 8);
        uint(&mut c, 0, (offset + 1440) as u64);
    }
    if c.len() + 6 > MAX_FRAME_BYTES {
        return Err(Error::TooLarge);
    }
    let mut f = Vec::with_capacity(c.len() + 6);
    f.extend(((c.len() + 4) as u16).to_be_bytes());
    f.extend(&c);
    f.extend(crc32(&c).to_be_bytes());
    Ok(f)
}
/// The one envelope parser behind every decoder (and the C++ `Decode`'s
/// envelope rules): frame integrity, canonical strictly-increasing keys,
/// major-first version check, bounded non-snapshot bodies, no trailing bytes.
/// Returns the envelope and the still-encoded body for the type's decoder.
fn parse_envelope(frame: &[u8]) -> Result<(Envelope, &[u8]), Error> {
    if frame.len() < 7 {
        return Err(Error::Truncated);
    }
    let length = u16::from_be_bytes([frame[0], frame[1]]) as usize;
    if length > MAX_FRAME_BYTES - 2 {
        return Err(Error::TooLarge);
    }
    if length + 2 != frame.len() {
        return Err(Error::Truncated);
    }
    let c = &frame[2..frame.len() - 4];
    if crc32(c) != u32::from_be_bytes(frame[frame.len() - 4..].try_into().unwrap()) {
        return Err(Error::BadCrc);
    }
    let mut p = 0;
    let count = read_uint(c, &mut p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let byte = |value: u64| u8::try_from(value).map_err(|_| Error::Malformed);
    let word = |value: u64| u32::try_from(value).map_err(|_| Error::Malformed);
    let (mut major, mut minor, mut kind) = (None, None, None);
    let (mut session, mut sequence, mut body) = (None, None, None);
    let (mut utc_ms, mut uncertainty, mut offset) = (None, None, None);
    let mut previous = None;
    for _ in 0..count {
        let key = read_uint(c, &mut p, 0).ok_or(Error::Malformed)?;
        // Canonical maps have strictly increasing keys: a repeat or a
        // reordering is malformed.
        if previous.is_some_and(|old| key <= old) {
            return Err(Error::Malformed);
        }
        previous = Some(key);
        let mut uint = || read_uint(c, &mut p, 0).ok_or(Error::Malformed);
        match key {
            0 => {
                let value = byte(uint()?)?;
                // Major is the first envelope key; a future major may change
                // everything after it, so nothing after it is interpreted.
                if value != 1 {
                    return Err(Error::UnsupportedMajor);
                }
                major = Some(value);
            }
            1 => minor = Some(byte(uint()?)?),
            2 => kind = Some(byte(uint()?)?),
            3 => {
                session = Some(bytes(c, &mut p, 16)?.try_into().unwrap());
            }
            4 => sequence = Some(word(uint()?)?),
            5 => {
                let kind = message_type(kind.ok_or(Error::Malformed)? as u64)?;
                let start = p;
                skip_body(c, &mut p)?;
                if kind != MessageType::Snapshot && p - start > 512 {
                    return Err(Error::TooLarge);
                }
                body = Some(&c[start..p]);
            }
            6 => utc_ms = Some(uint()?),
            7 => uncertainty = Some(word(uint()?)?),
            8 => {
                let wire = uint()?;
                if wire > 2880 {
                    return Err(Error::Malformed);
                }
                offset = Some(wire as i16 - 1440);
            }
            _ => skip_value(c, &mut p, 0)?,
        }
    }
    if p != c.len() || major.is_none() {
        return Err(Error::Malformed);
    }
    let envelope = Envelope {
        major: 1,
        minor: minor.ok_or(Error::Malformed)?,
        kind: message_type(kind.ok_or(Error::Malformed)? as u64)?,
        session: session.ok_or(Error::Malformed)?,
        sequence: sequence.ok_or(Error::Malformed)?,
        utc_ms,
        utc_uncertainty_ms: uncertainty,
        local_offset_minutes: offset,
    };
    Ok((envelope, body.ok_or(Error::Malformed)?))
}

fn decode_cpu_body(body: &[u8]) -> Result<Snapshot, Error> {
    let mut p = 0;
    let count = read_uint(body, &mut p, 5).ok_or(Error::Malformed)?;
    if count > 10 {
        return Err(Error::Malformed);
    }
    let mut cpu = [None; 5];
    let mut ram = None;
    let mut storage = None;
    let mut battery = None;
    let mut agents = None;
    let mut network = None;
    for _ in 0..count {
        let q = read_uint(body, &mut p, 0).ok_or(Error::Malformed)? as usize;
        if q == 5 {
            if ram.is_some() {
                return Err(Error::Malformed);
            }
            ram = Some(decode_ram_group(body, &mut p)?);
            continue;
        }
        if q == 6 {
            if storage.is_some() {
                return Err(Error::Malformed);
            }
            storage = Some(decode_storage_group(body, &mut p)?);
            continue;
        }
        if q == 7 {
            if battery.is_some() {
                return Err(Error::Malformed);
            }
            battery = Some(decode_battery_group(body, &mut p)?);
            continue;
        }
        if q == 8 {
            if agents.is_some() {
                return Err(Error::Malformed);
            }
            agents = Some(decode_agents_group(body, &mut p)?);
            continue;
        }
        if q == 9 {
            if network.is_some() {
                return Err(Error::Malformed);
            }
            network = Some(decode_network_group(body, &mut p)?);
            continue;
        }
        // Unknown optional fields are skipped like any other bounded value.
        if q > 4 {
            skip_value(body, &mut p, 0)?;
            continue;
        }
        if cpu[q].is_some() {
            return Err(Error::Malformed);
        }
        let value = read_uint(body, &mut p, 0).ok_or(Error::Malformed)?;
        if ((q == 0 || q == 3) && value > u8::MAX.into())
            || (q == 1 && value > 1000)
            || ((q == 2 || q == 4) && value > u32::MAX.into())
        {
            return Err(Error::Malformed);
        }
        cpu[q] = Some(value);
    }
    if p != body.len() {
        return Err(Error::Malformed);
    }
    let field = |q: usize| cpu[q].ok_or(Error::Malformed);
    let cpu = CpuSnapshot {
        availability: match field(0)? {
            0 => Availability::Available,
            1 => Availability::Unsupported,
            2 => Availability::Absent,
            3 => Availability::TemporarilyUnavailable,
            4 => Availability::Error,
            _ => return Err(Error::Malformed),
        },
        utilization_per_mille: field(1)? as u16,
        source_age_ms: field(2)? as u32,
        freshness: match field(3)? {
            0 => Freshness::Fresh,
            1 => Freshness::Stale,
            _ => return Err(Error::Malformed),
        },
        configuration_revision: field(4)? as u32,
    };
    Ok(Snapshot {
        cpu,
        ram,
        storage,
        battery,
        agents,
        network,
    })
}

/// A metric group: five required fields, unknown extra keys skipped, values
/// range-checked (mirrors the C++ `ReadRamGroup`).
fn decode_ram_group(input: &[u8], p: &mut usize) -> Result<RamSnapshot, Error> {
    let count = read_uint(input, p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let mut value = [None; 5];
    for _ in 0..count {
        let key = read_uint(input, p, 0).ok_or(Error::Malformed)? as usize;
        if key > 4 {
            skip_value(input, p, 0)?;
            continue;
        }
        if value[key].is_some() {
            return Err(Error::Malformed);
        }
        value[key] = Some(read_uint(input, p, 0).ok_or(Error::Malformed)?);
    }
    let field = |k: usize| value[k].ok_or(Error::Malformed);
    let (used, total) = (field(1)?, field(2)?);
    if used > total || field(3)? > u32::MAX.into() {
        return Err(Error::Malformed);
    }
    Ok(RamSnapshot {
        availability: match field(0)? {
            0 => Availability::Available,
            1 => Availability::Unsupported,
            2 => Availability::Absent,
            3 => Availability::TemporarilyUnavailable,
            4 => Availability::Error,
            _ => return Err(Error::Malformed),
        },
        used_bytes: used,
        total_bytes: total,
        source_age_ms: field(3)? as u32,
        freshness: match field(4)? {
            0 => Freshness::Fresh,
            1 => Freshness::Stale,
            _ => return Err(Error::Malformed),
        },
    })
}

fn availability_from(value: u64) -> Result<Availability, Error> {
    match value {
        0 => Ok(Availability::Available),
        1 => Ok(Availability::Unsupported),
        2 => Ok(Availability::Absent),
        3 => Ok(Availability::TemporarilyUnavailable),
        4 => Ok(Availability::Error),
        _ => Err(Error::Malformed),
    }
}

/// The Storage group: eight required fields, unknown extra keys skipped
/// (mirrors the C++ `ReadStorageGroup`).
fn decode_storage_group(input: &[u8], p: &mut usize) -> Result<StorageSnapshot, Error> {
    let count = read_uint(input, p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let mut value = [None; 8];
    for _ in 0..count {
        let key = read_uint(input, p, 0).ok_or(Error::Malformed)? as usize;
        if key > 7 {
            skip_value(input, p, 0)?;
            continue;
        }
        if value[key].is_some() {
            return Err(Error::Malformed);
        }
        value[key] = Some(read_uint(input, p, 0).ok_or(Error::Malformed)?);
    }
    let field = |k: usize| value[k].ok_or(Error::Malformed);
    let all: Vec<u64> = (0..8).map(field).collect::<Result<_, _>>()?;
    if all[2] > all[1] || all[3] > u32::MAX.into() || all[7] > u32::MAX.into() {
        return Err(Error::Malformed);
    }
    Ok(StorageSnapshot {
        capacity_availability: availability_from(all[0])?,
        total_bytes: all[1],
        available_bytes: all[2],
        capacity_age_ms: all[3] as u32,
        io_availability: availability_from(all[4])?,
        read_bytes_per_s: all[5],
        write_bytes_per_s: all[6],
        io_age_ms: all[7] as u32,
    })
}

/// The Battery group: ten required fields, unknown extra keys skipped
/// (mirrors the C++ `ReadBatteryGroup`).
fn decode_battery_group(input: &[u8], p: &mut usize) -> Result<BatterySnapshot, Error> {
    let count = read_uint(input, p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let mut value = [None; 10];
    for _ in 0..count {
        let key = read_uint(input, p, 0).ok_or(Error::Malformed)? as usize;
        if key > 9 {
            skip_value(input, p, 0)?;
            continue;
        }
        if value[key].is_some() {
            return Err(Error::Malformed);
        }
        value[key] = Some(read_uint(input, p, 0).ok_or(Error::Malformed)?);
    }
    let all: Vec<u64> = (0..10)
        .map(|k| value[k].ok_or(Error::Malformed))
        .collect::<Result<_, _>>()?;
    let max32 = u64::from(u32::MAX);
    let max16 = u64::from(u16::MAX);
    if all[1] > 1000
        || all[2] > 4
        || all[3] > max16
        || all[4] > max32
        || all[6] > 1000
        || all[7] > 3
        || all[8] > max16
        || all[9] > max32
    {
        return Err(Error::Malformed);
    }
    Ok(BatterySnapshot {
        charge_availability: availability_from(all[0])?,
        charge_per_mille: all[1] as u16,
        state: all[2] as u8,
        time_remaining_min: all[3] as u16,
        charge_age_ms: all[4] as u32,
        condition_availability: availability_from(all[5])?,
        max_capacity_per_mille: all[6] as u16,
        condition: all[7] as u8,
        cycle_count: all[8] as u16,
        condition_age_ms: all[9] as u32,
    })
}

/// The Agents group: five required fields, unknown extra keys skipped
/// (mirrors the C++ `ReadAgentsGroup`).
fn decode_agents_group(input: &[u8], p: &mut usize) -> Result<AgentsSnapshot, Error> {
    let count = read_uint(input, p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let mut value = [None; 5];
    for _ in 0..count {
        let key = read_uint(input, p, 0).ok_or(Error::Malformed)? as usize;
        if key > 4 {
            skip_value(input, p, 0)?;
            continue;
        }
        if value[key].is_some() {
            return Err(Error::Malformed);
        }
        value[key] = Some(read_uint(input, p, 0).ok_or(Error::Malformed)?);
    }
    let all: Vec<u64> = (0..5)
        .map(|k| value[k].ok_or(Error::Malformed))
        .collect::<Result<_, _>>()?;
    if all[1] > u64::from(u32::MAX) || all[2..].iter().any(|&s| s > 3) {
        return Err(Error::Malformed);
    }
    Ok(AgentsSnapshot {
        availability: availability_from(all[0])?,
        source_age_ms: all[1] as u32,
        state: [all[2] as u8, all[3] as u8, all[4] as u8],
    })
}

/// A definite-length byte string, at most the frame limit (mirrors `ReadBytes`).
fn read_byte_string(input: &[u8], p: &mut usize) -> Option<Vec<u8>> {
    let len = read_uint(input, p, 2)? as usize;
    if len > MAX_FRAME_BYTES || *p + len > input.len() {
        return None;
    }
    let bytes = input[*p..*p + len].to_vec();
    *p += len;
    Some(bytes)
}

/// The Network group: thirteen required fields (keys 10-12 are byte strings),
/// unknown extra keys skipped (mirrors the C++ `ReadNetworkGroup`).
fn decode_network_group(input: &[u8], p: &mut usize) -> Result<NetworkSnapshot, Error> {
    let count = read_uint(input, p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let mut value = [None; 10];
    let mut bytes: [Option<Vec<u8>>; 3] = [None, None, None];
    for _ in 0..count {
        let key = read_uint(input, p, 0).ok_or(Error::Malformed)? as usize;
        if key > 12 {
            skip_value(input, p, 0)?;
            continue;
        }
        if key >= 10 {
            if bytes[key - 10].is_some() {
                return Err(Error::Malformed);
            }
            bytes[key - 10] = Some(read_byte_string(input, p).ok_or(Error::Malformed)?);
        } else {
            if value[key].is_some() {
                return Err(Error::Malformed);
            }
            value[key] = Some(read_uint(input, p, 0).ok_or(Error::Malformed)?);
        }
    }
    let all: Vec<u64> = (0..10)
        .map(|k| value[k].ok_or(Error::Malformed))
        .collect::<Result<_, _>>()?;
    let [station, ssid, bssid] = bytes;
    let (station, ssid, bssid) = (
        station.ok_or(Error::Malformed)?,
        ssid.ok_or(Error::Malformed)?,
        bssid.ok_or(Error::Malformed)?,
    );
    let mac = |b: &[u8]| -> Result<Option<[u8; 6]>, Error> {
        match b.len() {
            0 => Ok(None),
            6 => Ok(Some(b.try_into().unwrap())),
            _ => Err(Error::Malformed),
        }
    };
    if all[1] > u64::from(u32::MAX)
        || all[2] > 4
        || all[3] > 1
        || all[6] > 6
        || all[7] > u64::from(u32::MAX)
        || all[8] > 2
        || all[9] > 255
    {
        return Err(Error::Malformed);
    }
    let snapshot = NetworkSnapshot {
        traffic_availability: availability_from(all[0])?,
        traffic_age_ms: all[1] as u32,
        route_kind: match all[2] {
            0 => RouteKind::Unknown,
            1 => RouteKind::Wifi,
            2 => RouteKind::Ethernet,
            3 => RouteKind::Other,
            _ => RouteKind::None,
        },
        via_tunnel: all[3] == 1,
        rx_bytes_per_s: all[4],
        tx_bytes_per_s: all[5],
        context_availability: match all[6] {
            0 => AssociationContext::Available,
            1 => AssociationContext::PermissionNotRequested,
            2 => AssociationContext::Denied,
            3 => AssociationContext::Restricted,
            4 => AssociationContext::AuthorizedUnavailable,
            5 => AssociationContext::Unsupported,
            _ => AssociationContext::Error,
        },
        context_age_ms: all[7] as u32,
        coverage: match all[8] {
            0 => WifiCoverage::Unknown,
            1 => WifiCoverage::Observable,
            _ => WifiCoverage::OutsideSupported,
        },
        channel: all[9] as u8,
        station: mac(&station)?,
        ssid,
        bssid: mac(&bssid)?,
    };
    if !snapshot.is_valid() {
        return Err(Error::Malformed);
    }
    Ok(snapshot)
}

/// Decodes a complete snapshot frame: CPU plus any metric groups.
pub fn decode_snapshot(frame: &[u8]) -> Result<(Envelope, Snapshot), Error> {
    let (envelope, body) = parse_envelope(frame)?;
    if envelope.kind != MessageType::Snapshot {
        return Err(Error::Malformed);
    }
    Ok((envelope, decode_cpu_body(body)?))
}

/// Decodes a snapshot frame's CPU part; any RAM group is still validated.
pub fn decode_cpu(frame: &[u8]) -> Result<(Envelope, CpuSnapshot), Error> {
    let (envelope, snapshot) = decode_snapshot(frame)?;
    Ok((envelope, snapshot.cpu))
}

fn message_type(value: u64) -> Result<MessageType, Error> {
    match value {
        1 => Ok(MessageType::Hello),
        2 => Ok(MessageType::HelloReply),
        3 => Ok(MessageType::Pair),
        4 => Ok(MessageType::Snapshot),
        5 => Ok(MessageType::Command),
        6 => Ok(MessageType::CommandReply),
        7 => Ok(MessageType::Error),
        _ => Err(Error::Malformed),
    }
}

fn bytes(input: &[u8], p: &mut usize, expected_len: usize) -> Result<Vec<u8>, Error> {
    let len = read_uint(input, p, 2).ok_or(Error::Malformed)? as usize;
    if len != expected_len || *p + len > input.len() {
        return Err(Error::Malformed);
    }
    let result = input[*p..*p + len].to_vec();
    *p += len;
    Ok(result)
}

// Unknown fields are permitted only where the core protocol says they are
// optional. This recognises all definite CBOR values and bounds nesting so a
// malformed extension cannot cause unbounded parser work.
/// Skips a message body to find where it ends; the typed decoders then
/// enforce the exact rules. A body's own map is not a nesting level, and
/// neither is a metric group map directly inside it: an unknown value's depth
/// is counted from that value, wherever it sits. Deliberately at least as
/// permissive as any typed decoder, so it never rejects what they accept.
fn skip_body(input: &[u8], p: &mut usize) -> Result<(), Error> {
    match input.get(*p).map(|first| first >> 5) {
        Some(5) => {
            let count = read_uint(input, p, 5).ok_or(Error::Malformed)?;
            if count > 16 {
                return Err(Error::Malformed);
            }
            for _ in 0..count {
                skip_value(input, p, 0)?;
                match input.get(*p).map(|first| first >> 5) {
                    Some(5) => {
                        let inner = read_uint(input, p, 5).ok_or(Error::Malformed)?;
                        if inner > 16 {
                            return Err(Error::Malformed);
                        }
                        for _ in 0..inner * 2 {
                            skip_value(input, p, 0)?;
                        }
                    }
                    _ => skip_value(input, p, 0)?,
                }
            }
            Ok(())
        }
        _ => skip_value(input, p, 0),
    }
}

fn skip_value(input: &[u8], p: &mut usize, depth: u8) -> Result<(), Error> {
    if depth >= 4 {
        return Err(Error::Malformed);
    }
    let first = *input.get(*p).ok_or(Error::Malformed)?;
    let major = first >> 5;
    match major {
        0 | 1 => {
            read_uint(input, p, major).ok_or(Error::Malformed)?;
        }
        2 | 3 => {
            let len = read_uint(input, p, major).ok_or(Error::Malformed)? as usize;
            if *p + len > input.len() {
                return Err(Error::Malformed);
            }
            *p += len;
        }
        4 | 5 => {
            let count = read_uint(input, p, major).ok_or(Error::Malformed)?;
            if count > 16 {
                return Err(Error::Malformed);
            }
            for _ in 0..count * if major == 5 { 2 } else { 1 } {
                skip_value(input, p, depth + 1)?;
            }
        }
        // Tags and simple/float values are not part of the deterministic
        // profile, whether or not the decoder knows the field.
        _ => return Err(Error::Malformed),
    }
    Ok(())
}

fn encode_frame(envelope: &Envelope, body: &[u8]) -> Result<Vec<u8>, Error> {
    if body.len() > 512 {
        return Err(Error::TooLarge);
    }
    let mut c = vec![
        0xa6 + envelope.utc_ms.is_some() as u8
            + envelope.utc_uncertainty_ms.is_some() as u8
            + envelope.local_offset_minutes.is_some() as u8,
    ];
    for (key, value) in [
        (0, envelope.major as u64),
        (1, envelope.minor as u64),
        (2, envelope.kind as u64),
    ] {
        uint(&mut c, 0, key);
        uint(&mut c, 0, value);
    }
    uint(&mut c, 0, 3);
    uint(&mut c, 2, 16);
    c.extend(envelope.session);
    uint(&mut c, 0, 4);
    uint(&mut c, 0, envelope.sequence as u64);
    uint(&mut c, 0, 5);
    c.extend(body);
    if let Some(value) = envelope.utc_ms {
        uint(&mut c, 0, 6);
        uint(&mut c, 0, value);
    }
    if let Some(value) = envelope.utc_uncertainty_ms {
        uint(&mut c, 0, 7);
        uint(&mut c, 0, value as u64);
    }
    if let Some(offset) = envelope.local_offset_minutes {
        if !(-1440..=1440).contains(&offset) {
            return Err(Error::Malformed);
        }
        uint(&mut c, 0, 8);
        uint(&mut c, 0, (offset + 1440) as u64);
    }
    if c.len() + 6 > MAX_FRAME_BYTES {
        return Err(Error::TooLarge);
    }
    let mut frame = Vec::with_capacity(c.len() + 6);
    frame.extend(((c.len() + 4) as u16).to_be_bytes());
    frame.extend(&c);
    frame.extend(crc32(&c).to_be_bytes());
    Ok(frame)
}

/// Encodes a structured control body in a bounded canonical-CBOR frame.
pub fn encode_control(envelope: &Envelope, body: &ControlBody) -> Result<Vec<u8>, Error> {
    let mut c = Vec::new();
    match body {
        ControlBody::Hello(value) | ControlBody::HelloReply(value) => {
            if !matches!(
                (envelope.kind, body),
                (MessageType::Hello, ControlBody::Hello(_))
                    | (MessageType::HelloReply, ControlBody::HelloReply(_))
            ) {
                return Err(Error::Malformed);
            }
            c.push(0xa4 + value.utc_ms.is_some() as u8 + value.utc_uncertainty_ms.is_some() as u8);
            for (key, item) in [
                (0, value.supported_major as u64),
                (1, value.supported_minor as u64),
                (2, value.capabilities as u64),
                (3, value.configuration_revision as u64),
            ] {
                uint(&mut c, 0, key);
                uint(&mut c, 0, item);
            }
            if let Some(item) = value.utc_ms {
                uint(&mut c, 0, 4);
                uint(&mut c, 0, item);
            }
            if let Some(item) = value.utc_uncertainty_ms {
                uint(&mut c, 0, 5);
                uint(&mut c, 0, item as u64);
            }
        }
        ControlBody::Pair(value) => {
            if envelope.kind != MessageType::Pair
                || !(100_000..=999_999).contains(&value.matching_code)
            {
                return Err(Error::Malformed);
            }
            c.push(0xa3);
            uint(&mut c, 0, 0);
            uint(&mut c, 2, 16);
            c.extend(value.pairing_identity);
            uint(&mut c, 0, 1);
            uint(&mut c, 2, 16);
            c.extend(value.intended_peer_identity);
            uint(&mut c, 0, 2);
            uint(&mut c, 0, value.matching_code as u64);
        }
        ControlBody::Command(value) => {
            if envelope.kind != MessageType::Command {
                return Err(Error::Malformed);
            }
            c.push(0xa2);
            uint(&mut c, 0, 0);
            uint(&mut c, 0, value.command_identity as u64);
            uint(&mut c, 0, 1);
            uint(&mut c, 0, value.operation as u64);
        }
        ControlBody::Error(value) => {
            if envelope.kind != MessageType::Error || value.len() > 512 - 3 {
                return Err(Error::Malformed);
            }
            uint(&mut c, 2, value.len() as u64);
            c.extend(value);
        }
        ControlBody::CommandReply(value) => {
            if envelope.kind != MessageType::CommandReply {
                return Err(Error::Malformed);
            }
            c.push(0xa3);
            uint(&mut c, 0, 0);
            uint(&mut c, 0, value.command_identity as u64);
            uint(&mut c, 0, 1);
            uint(&mut c, 0, value.operation as u64);
            uint(&mut c, 0, 2);
            uint(&mut c, 0, value.result as u64);
        }
    }
    encode_frame(envelope, &c)
}

/// A decoded frame of any message type.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Snapshot(Envelope, Snapshot),
    Control(Envelope, ControlBody),
}

/// Decodes any frame, choosing the body decoder from the envelope's type.
pub fn decode(frame: &[u8]) -> Result<Message, Error> {
    let (envelope, body) = parse_envelope(frame)?;
    if envelope.kind == MessageType::Snapshot {
        let cpu = decode_cpu_body(body)?;
        Ok(Message::Snapshot(envelope, cpu))
    } else {
        let control = decode_control_body(envelope.kind, body)?;
        Ok(Message::Control(envelope, control))
    }
}

/// Decodes a non-snapshot control frame, including its typed body.
pub fn decode_control(frame: &[u8]) -> Result<(Envelope, ControlBody), Error> {
    let (envelope, body) = parse_envelope(frame)?;
    if envelope.kind == MessageType::Snapshot {
        return Err(Error::Malformed);
    }
    let control = decode_control_body(envelope.kind, body)?;
    Ok((envelope, control))
}

fn op(value: u64) -> Result<CommandOperation, Error> {
    match value {
        1 => Ok(CommandOperation::Unpair),
        2 => Ok(CommandOperation::CalibrationReset),
        _ => Err(Error::Malformed),
    }
}
fn result(value: u64) -> Result<CommandResult, Error> {
    match value {
        0 => Ok(CommandResult::Applied),
        1 => Ok(CommandResult::AlreadyApplied),
        2 => Ok(CommandResult::Rejected),
        _ => Err(Error::Malformed),
    }
}

fn decode_control_body(kind: MessageType, c: &[u8]) -> Result<ControlBody, Error> {
    let mut p = 0;
    if kind == MessageType::Error {
        let len = read_uint(c, &mut p, 2).ok_or(Error::Malformed)? as usize;
        if p + len != c.len() {
            return Err(Error::Malformed);
        }
        return Ok(ControlBody::Error(c[p..].to_vec()));
    }
    // Keys above a type's last known key are unknown optional fields.
    let known_max = match kind {
        MessageType::Hello | MessageType::HelloReply => 5,
        MessageType::Pair | MessageType::CommandReply => 2,
        MessageType::Command => 1,
        _ => return Err(Error::Malformed),
    };
    let count = read_uint(c, &mut p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let mut fields: [Option<u64>; 6] = [None; 6];
    let mut identities: [Option<[u8; 16]>; 2] = [None, None];
    let mut prior = None;
    for _ in 0..count {
        let key = read_uint(c, &mut p, 0).ok_or(Error::Malformed)?;
        if prior.is_some_and(|old| key <= old) {
            return Err(Error::Malformed);
        }
        prior = Some(key);
        if key > known_max {
            skip_value(c, &mut p, 0)?;
            continue;
        }
        let key = key as usize;
        if kind == MessageType::Pair && key < 2 {
            identities[key] = Some(bytes(c, &mut p, 16)?.try_into().unwrap());
        } else {
            fields[key] = Some(read_uint(c, &mut p, 0).ok_or(Error::Malformed)?);
        }
    }
    if p != c.len() {
        return Err(Error::Malformed);
    }
    match kind {
        MessageType::Hello | MessageType::HelloReply => {
            let value = HelloBody {
                supported_major: fields[0]
                    .ok_or(Error::Malformed)?
                    .try_into()
                    .map_err(|_| Error::Malformed)?,
                supported_minor: fields[1]
                    .ok_or(Error::Malformed)?
                    .try_into()
                    .map_err(|_| Error::Malformed)?,
                capabilities: fields[2]
                    .ok_or(Error::Malformed)?
                    .try_into()
                    .map_err(|_| Error::Malformed)?,
                configuration_revision: fields[3]
                    .ok_or(Error::Malformed)?
                    .try_into()
                    .map_err(|_| Error::Malformed)?,
                utc_ms: fields[4],
                utc_uncertainty_ms: fields[5]
                    .map(|v| v.try_into().map_err(|_| Error::Malformed))
                    .transpose()?,
            };
            Ok(if kind == MessageType::Hello {
                ControlBody::Hello(value)
            } else {
                ControlBody::HelloReply(value)
            })
        }
        MessageType::Pair => {
            let matching_code: u32 = fields[2]
                .ok_or(Error::Malformed)?
                .try_into()
                .map_err(|_| Error::Malformed)?;
            if !(100_000..=999_999).contains(&matching_code) {
                return Err(Error::Malformed);
            }
            Ok(ControlBody::Pair(PairBody {
                pairing_identity: identities[0].ok_or(Error::Malformed)?,
                intended_peer_identity: identities[1].ok_or(Error::Malformed)?,
                matching_code,
            }))
        }
        MessageType::Command => Ok(ControlBody::Command(CommandBody {
            command_identity: fields[0]
                .ok_or(Error::Malformed)?
                .try_into()
                .map_err(|_| Error::Malformed)?,
            operation: op(fields[1].ok_or(Error::Malformed)?)?,
        })),
        MessageType::CommandReply => Ok(ControlBody::CommandReply(CommandReplyBody {
            command_identity: fields[0]
                .ok_or(Error::Malformed)?
                .try_into()
                .map_err(|_| Error::Malformed)?,
            operation: op(fields[1].ok_or(Error::Malformed)?)?,
            result: result(fields[2].ok_or(Error::Malformed)?)?,
        })),
        _ => Err(Error::Malformed),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(name: &str) -> Vec<u8> {
        let path = format!(
            "{}/../../protocol/fixtures/{name}",
            env!("CARGO_MANIFEST_DIR")
        );
        let hex = std::fs::read_to_string(&path).unwrap_or_else(|_| panic!("missing {path}"));
        hex.lines()
            .filter(|line| !line.starts_with('#'))
            .collect::<String>()
            .trim()
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect()
    }

    #[test]
    fn parity_matrix_has_the_declared_cpu_decoder_outcomes() {
        for line in include_str!("../../../protocol/fixtures/parity-matrix.txt").lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let fields: Vec<_> = line.split('|').collect();
            assert_eq!(fields.len(), 4, "invalid parity-matrix row: {line}");
            let actual = match fields[2] {
                "cpu" => decode_cpu(&fixture(fields[1])).map(|_| ()),
                "control" => decode_control(&fixture(fields[1])).map(|_| ()),
                "any" => decode(&fixture(fields[1])).map(|_| ()),
                other => panic!("unknown decoder {other}"),
            };
            match fields[3] {
                "none" => assert!(actual.is_ok(), "{}: {actual:?}", fields[0]),
                "bad-crc" => assert_eq!(actual, Err(Error::BadCrc), "{}", fields[0]),
                "unsupported-major" => {
                    assert_eq!(actual, Err(Error::UnsupportedMajor), "{}", fields[0])
                }
                "malformed" => assert_eq!(actual, Err(Error::Malformed), "{}", fields[0]),
                "truncated" => assert_eq!(actual, Err(Error::Truncated), "{}", fields[0]),
                "too-large" => assert_eq!(actual, Err(Error::TooLarge), "{}", fields[0]),
                other => panic!("unknown expected result {other}"),
            }
        }
    }

    #[test]
    fn golden_snapshot_and_bad_crc() {
        let e = Envelope {
            major: 1,
            minor: 0,
            kind: MessageType::Snapshot,
            session: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
            sequence: 7,
            utc_ms: None,
            utc_uncertainty_ms: None,
            local_offset_minutes: None,
        };
        let c = CpuSnapshot {
            availability: Availability::Available,
            utilization_per_mille: 723,
            source_age_ms: 1000,
            freshness: Freshness::Fresh,
            configuration_revision: 4,
        };
        let f = encode_cpu(&e, &c).unwrap();
        let golden = include_str!("../../../protocol/fixtures/cpu-snapshot-v1.hex")
            .lines()
            .filter(|line| !line.starts_with('#'))
            .collect::<String>();
        let golden = (0..golden.trim().len())
            .step_by(2)
            .map(|index| u8::from_str_radix(&golden.trim()[index..index + 2], 16).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(golden, f);
        assert_eq!(decode_cpu(&f), Ok((e.clone(), c.clone())));

        let mut timed = e.clone();
        timed.utc_ms = Some(1_789_765_432_123);
        timed.utc_uncertainty_ms = Some(42);
        let timed_frame = encode_cpu(&timed, &c).unwrap();
        assert_eq!(decode_cpu(&timed_frame), Ok((timed, c.clone())));

        for prefix in 0..f.len() {
            assert_eq!(decode_cpu(&f[..prefix]), Err(Error::Truncated));
        }
        let mut incompatible = f.clone();
        incompatible[4] = 2;
        let crc = crc32(&incompatible[2..incompatible.len() - 4]);
        let end = incompatible.len();
        incompatible[end - 4..].copy_from_slice(&crc.to_be_bytes());
        assert_eq!(decode_cpu(&incompatible), Err(Error::UnsupportedMajor));
        let mut oversized = f.clone();
        oversized[..2].copy_from_slice(&2048u16.to_be_bytes());
        assert_eq!(decode_cpu(&oversized), Err(Error::TooLarge));
        let mut invalid_availability = f.clone();
        // The canonical vector's CPU availability is the value at offset 32.
        invalid_availability[32] = 5;
        let crc = crc32(&invalid_availability[2..invalid_availability.len() - 4]);
        let end = invalid_availability.len();
        invalid_availability[end - 4..].copy_from_slice(&crc.to_be_bytes());
        assert_eq!(decode_cpu(&invalid_availability), Err(Error::Malformed));
        let mut noncanonical = f.clone();
        noncanonical[32] = 0x18;
        noncanonical.insert(33, 0);
        let length = (noncanonical.len() - 2) as u16;
        noncanonical[..2].copy_from_slice(&length.to_be_bytes());
        let crc = crc32(&noncanonical[2..noncanonical.len() - 4]);
        let end = noncanonical.len();
        noncanonical[end - 4..].copy_from_slice(&crc.to_be_bytes());
        assert_eq!(decode_cpu(&noncanonical), Err(Error::Malformed));
        let mut bad = f;
        *bad.last_mut().unwrap() ^= 1;
        assert_eq!(decode_cpu(&bad), Err(Error::BadCrc));
    }

    fn control_envelope(kind: MessageType) -> Envelope {
        Envelope {
            major: 1,
            minor: 0,
            kind,
            session: [9; 16],
            sequence: 1,
            utc_ms: None,
            utc_uncertainty_ms: None,
            local_offset_minutes: None,
        }
    }

    #[test]
    fn control_bodies_round_trip_and_validate() {
        let hello = HelloBody {
            supported_major: 1,
            supported_minor: 0,
            capabilities: 0x12,
            configuration_revision: 8,
            utc_ms: Some(1_789_765_432_123),
            utc_uncertainty_ms: Some(42),
        };
        let pair = PairBody {
            pairing_identity: [1; 16],
            intended_peer_identity: [0; 16],
            matching_code: 123_456,
        };
        let command = CommandBody {
            command_identity: 99,
            operation: CommandOperation::CalibrationReset,
        };
        let reply = CommandReplyBody {
            command_identity: 99,
            operation: CommandOperation::CalibrationReset,
            result: CommandResult::AlreadyApplied,
        };
        for (envelope, body) in [
            (
                control_envelope(MessageType::Hello),
                ControlBody::Hello(hello),
            ),
            (
                control_envelope(MessageType::HelloReply),
                ControlBody::HelloReply(HelloBody {
                    supported_major: 1,
                    supported_minor: 0,
                    capabilities: 0,
                    configuration_revision: 9,
                    utc_ms: None,
                    utc_uncertainty_ms: None,
                }),
            ),
            (control_envelope(MessageType::Pair), ControlBody::Pair(pair)),
            (
                control_envelope(MessageType::Command),
                ControlBody::Command(command),
            ),
            (
                control_envelope(MessageType::CommandReply),
                ControlBody::CommandReply(reply),
            ),
        ] {
            let frame = encode_control(&envelope, &body).unwrap();
            assert_eq!(decode_control(&frame), Ok((envelope, body)));
        }

        let invalid_pair = PairBody {
            pairing_identity: [1; 16],
            intended_peer_identity: [0; 16],
            matching_code: 99_999,
        };
        assert_eq!(
            encode_control(
                &control_envelope(MessageType::Pair),
                &ControlBody::Pair(invalid_pair)
            ),
            Err(Error::Malformed)
        );
        assert_eq!(
            encode_control(
                &control_envelope(MessageType::Hello),
                &ControlBody::Command(CommandBody {
                    command_identity: 1,
                    operation: CommandOperation::Unpair
                })
            ),
            Err(Error::Malformed)
        );
    }

    #[test]
    fn control_decoder_rejects_noncanonical_and_wrong_body() {
        let envelope = control_envelope(MessageType::Command);
        let body = ControlBody::Command(CommandBody {
            command_identity: 7,
            operation: CommandOperation::Unpair,
        });
        let frame = encode_control(&envelope, &body).unwrap();
        // The command identity is canonically encoded as the single byte 7.
        let index = frame.iter().position(|value| *value == 7).unwrap();
        let mut noncanonical = frame.clone();
        noncanonical[index] = 0x18;
        noncanonical.insert(index + 1, 7);
        let length = (noncanonical.len() - 2) as u16;
        noncanonical[..2].copy_from_slice(&length.to_be_bytes());
        let end = noncanonical.len();
        let crc = crc32(&noncanonical[2..end - 4]);
        noncanonical[end - 4..].copy_from_slice(&crc.to_be_bytes());
        assert_eq!(decode_control(&noncanonical), Err(Error::Malformed));

        let mut truncated = frame;
        truncated.pop();
        assert_eq!(decode_control(&truncated), Err(Error::Truncated));
    }

    #[test]
    fn session_receiver_rejects_reconnect_until_reset() {
        let mut receiver = SessionReceiver::default();
        let first = control_envelope(MessageType::Hello);
        assert_eq!(receiver.accept(&first), Ok(()));
        assert_eq!(receiver.accept(&first), Err(Error::StaleSequence));
        let mut reconnect = first.clone();
        reconnect.session[0] = 10;
        assert_eq!(receiver.accept(&reconnect), Err(Error::WrongSession));
        receiver.reset(reconnect.session);
        assert_eq!(receiver.accept(&reconnect), Ok(()));
    }

    #[test]
    fn session_matrix_matches_the_firmware_receiver() {
        let mut receiver = SessionReceiver::default();
        for line in include_str!("../../../protocol/fixtures/session-parity-matrix.txt").lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let f: Vec<_> = line.split('|').collect();
            let mut session = [0u8; 16];
            session[0] = f[1].parse().unwrap();
            if f[0] == "reset" {
                receiver.reset(session);
                continue;
            }
            let mut envelope = control_envelope(MessageType::Hello);
            envelope.session = session;
            envelope.sequence = f[2].parse().unwrap();
            let expected = match f[3] {
                "none" => Ok(()),
                "stale-sequence" => Err(Error::StaleSequence),
                "wrong-session" => Err(Error::WrongSession),
                other => panic!("unknown expected result {other}"),
            };
            assert_eq!(receiver.accept(&envelope), expected, "{line}");
        }
    }

    #[test]
    fn stream_framer_resynchronizes_across_fragments_and_damage() {
        let good = fixture("cpu-snapshot-v1.hex");
        let mut stream = FrameStream::default();
        let mut frames = 0;
        for byte in good.iter().chain(good.iter()) {
            stream.push(&[*byte]);
            while let Some(frame) = stream.next_frame() {
                assert!(decode_cpu(&frame).is_ok());
                frames += 1;
            }
        }
        assert_eq!(
            (frames, stream.dropped_bytes(), stream.buffered_bytes()),
            (2, 0, 0)
        );

        let mut stream = FrameStream::default();
        let mut wire = vec![0xff, 0xff, 0, 0, 8];
        wire.extend(&good);
        stream.push(&wire);
        assert_eq!(stream.next_frame(), Some(good.clone()));
        assert_eq!(stream.next_frame(), None);

        // A damaged frame is skipped. Rescanning its bytes can find a
        // plausible length that waits for up to 2,046 bytes; the adapter's
        // idle gap (`drop_leading_byte`) clears that, so here more traffic does.
        let mut damaged = good.clone();
        damaged[10] ^= 0x40;
        let mut stream = FrameStream::default();
        stream.push(&damaged);
        let mut recovered = 0;
        for _ in 0..80 {
            stream.push(&good);
            while let Some(frame) = stream.next_frame() {
                assert!(decode_cpu(&frame).is_ok());
                recovered += 1;
            }
        }
        assert!(recovered >= 60 && stream.dropped_bytes() > 0, "{recovered}");

        // Valid length and CRC, rejected body: consumed whole, then resumes.
        let bad = fixture("cpu-snapshot-v1-incompatible-major.hex");
        let mut stream = FrameStream::default();
        stream.push(&bad);
        stream.push(&good);
        let first = stream.next_frame().unwrap();
        assert_eq!(decode_cpu(&first), Err(Error::UnsupportedMajor));
        assert_eq!(stream.dropped_bytes(), 0);
        assert_eq!(stream.next_frame(), Some(good));
    }

    #[test]
    fn encoders_reproduce_independently_generated_vectors() {
        let session: [u8; 16] = core::array::from_fn(|i| i as u8);
        let envelope = |kind| Envelope {
            major: 1,
            minor: 0,
            kind,
            session,
            sequence: 1,
            utc_ms: None,
            utc_uncertainty_ms: None,
            local_offset_minutes: None,
        };
        let hello = HelloBody {
            supported_major: 1,
            supported_minor: 0,
            capabilities: 0x21,
            configuration_revision: 9,
            utc_ms: None,
            utc_uncertainty_ms: None,
        };
        assert_eq!(
            encode_control(
                &envelope(MessageType::HelloReply),
                &ControlBody::HelloReply(hello)
            )
            .unwrap(),
            fixture("hello-reply-v1.hex")
        );
        assert_eq!(
            encode_control(
                &envelope(MessageType::Error),
                &ControlBody::Error(vec![1, 2, 3])
            )
            .unwrap(),
            fixture("error-v1.hex")
        );
        assert_eq!(
            decode_control(&fixture("error-v1.hex")).unwrap().1,
            ControlBody::Error(vec![1, 2, 3])
        );
    }

    #[test]
    fn ram_group_round_trips_and_decodes_to_the_golden_values() {
        let (_, golden) = decode_snapshot(&fixture("ram-snapshot-v1.hex")).unwrap();
        let ram = golden.ram.expect("ram group");
        assert_eq!(ram.used_bytes, 8 << 30);
        assert_eq!(ram.total_bytes, 16 << 30);
        assert_eq!(ram.source_age_ms, 250);
        assert_eq!(ram.availability, Availability::Available);
        assert_eq!(golden.cpu.utilization_per_mille, 723);
        // Re-encoding the decoded snapshot reproduces the fixture bytes.
        let (envelope, snapshot) = decode_snapshot(&fixture("ram-snapshot-v1.hex")).unwrap();
        assert_eq!(
            encode_snapshot(&envelope, &snapshot.cpu, snapshot.ram.as_ref()).unwrap(),
            fixture("ram-snapshot-v1.hex")
        );
        // A CPU-only frame has no group.
        assert_eq!(
            decode_snapshot(&fixture("cpu-snapshot-v1.hex"))
                .unwrap()
                .1
                .ram,
            None
        );
        // The encoder refuses an impossible group.
        let bad = RamSnapshot {
            used_bytes: 2,
            total_bytes: 1,
            ..ram
        };
        assert_eq!(
            encode_snapshot(&envelope, &snapshot.cpu, Some(&bad)),
            Err(Error::Malformed)
        );
    }

    #[test]
    fn storage_group_round_trips_beside_ram_and_stays_within_its_bound() {
        let (envelope, golden) = decode_snapshot(&fixture("storage-snapshot-v1.hex")).unwrap();
        let g = golden.storage.clone().expect("storage group");
        assert_eq!(
            (g.total_bytes, g.available_bytes),
            (500_000_000_000, 200_000_000_000)
        );
        assert_eq!(
            (g.read_bytes_per_s, g.write_bytes_per_s),
            (12_300_000, 4_500_000)
        );
        assert_eq!((g.capacity_age_ms, g.io_age_ms), (1500, 250));
        assert_eq!(golden.ram, None);
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    storage: Some(g.clone()),
                    ..Snapshot::new(golden.cpu.clone())
                }
            )
            .unwrap(),
            fixture("storage-snapshot-v1.hex")
        );
        let (envelope, both) =
            decode_snapshot(&fixture("storage-snapshot-v1-with-ram.hex")).unwrap();
        assert!(both.ram.is_some() && both.storage.is_some());
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    ram: both.ram.clone(),
                    storage: both.storage.clone(),
                    ..Snapshot::new(both.cpu.clone())
                }
            )
            .unwrap(),
            fixture("storage-snapshot-v1-with-ram.hex")
        );
        // The largest group: every count at its maximum, well under the frame limit.
        let max = fixture("storage-snapshot-v1-max-u64.hex");
        assert!(max.len() < 2048);
        let (envelope, snap) = decode_snapshot(&max).unwrap();
        let without = encode_groups(&envelope, &Snapshot::new(snap.cpu.clone())).unwrap();
        // key + map header + eight keys + 2 availabilities + 4 x 9 + 2 x 5 = 58 bytes
        assert_eq!(max.len() - without.len(), 58);
        let bad = StorageSnapshot {
            available_bytes: g.total_bytes + 1,
            ..g
        };
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    storage: Some(bad.clone()),
                    ..Snapshot::new(snap.cpu.clone())
                }
            ),
            Err(Error::Malformed)
        );
    }

    #[test]
    fn battery_group_round_trips_and_stays_within_its_bound() {
        let (envelope, golden) = decode_snapshot(&fixture("battery-snapshot-v1.hex")).unwrap();
        let g = golden.battery.clone().expect("battery group");
        assert_eq!(
            (g.charge_availability, g.charge_per_mille, g.state),
            (Availability::Available, 870, 1)
        );
        assert_eq!((g.time_remaining_min, g.charge_age_ms), (95, 250));
        assert_eq!(
            (g.max_capacity_per_mille, g.condition, g.cycle_count),
            (910, 1, 312)
        );
        assert_eq!(g.condition_age_ms, 1_800_000);
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    battery: Some(g.clone()),
                    ..Snapshot::new(golden.cpu.clone())
                }
            )
            .unwrap(),
            fixture("battery-snapshot-v1.hex")
        );
        let (envelope, all) =
            decode_snapshot(&fixture("battery-snapshot-v1-with-groups.hex")).unwrap();
        assert!(all.ram.is_some() && all.storage.is_some() && all.battery.is_some());
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    ram: all.ram.clone(),
                    storage: all.storage.clone(),
                    battery: all.battery.clone(),
                    ..Snapshot::new(all.cpu.clone())
                }
            )
            .unwrap(),
            fixture("battery-snapshot-v1-with-groups.hex")
        );
        // Batteryless host: explicitly absent, never a zero charge.
        let (_, none) = decode_snapshot(&fixture("battery-snapshot-v1-absent.hex")).unwrap();
        let absent = none.battery.unwrap();
        assert_eq!(absent.charge_availability, Availability::Absent);
        assert_eq!(absent.condition_availability, Availability::Absent);
        // The largest group: every value at its maximum, under the 64-byte budget.
        let max = fixture("battery-snapshot-v1-max.hex");
        assert!(max.len() < 2048);
        let (envelope, snap) = decode_snapshot(&max).unwrap();
        let without = encode_groups(&envelope, &Snapshot::new(snap.cpu.clone())).unwrap();
        // key + map header + ten keys + 4 availability/state/condition bytes
        // + 4 x 3-byte (per-mille, sentinels) + 2 x 5-byte ages = 38 bytes
        assert_eq!(max.len() - without.len(), 38);
        let bad = BatterySnapshot {
            charge_per_mille: 1001,
            ..snap.battery.clone().unwrap()
        };
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    battery: Some(bad.clone()),
                    ..Snapshot::new(snap.cpu.clone())
                }
            ),
            Err(Error::Malformed)
        );
    }

    #[test]
    fn agents_group_round_trips_and_stays_within_its_bound() {
        let (envelope, golden) = decode_snapshot(&fixture("agents-snapshot-v1.hex")).unwrap();
        let g = golden.agents.clone().expect("agents group");
        assert_eq!(
            (g.availability, g.source_age_ms, g.state),
            (Availability::Available, 250, [0, 1, 0])
        );
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    agents: Some(g.clone()),
                    ..Snapshot::new(golden.cpu.clone())
                }
            )
            .unwrap(),
            fixture("agents-snapshot-v1.hex")
        );
        let (envelope, all) =
            decode_snapshot(&fixture("agents-snapshot-v1-with-groups.hex")).unwrap();
        assert!(
            all.ram.is_some()
                && all.storage.is_some()
                && all.battery.is_some()
                && all.agents.is_some()
        );
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    ram: all.ram.clone(),
                    storage: all.storage.clone(),
                    battery: all.battery.clone(),
                    agents: all.agents.clone(),
                    ..Snapshot::new(all.cpu.clone())
                }
            )
            .unwrap(),
            fixture("agents-snapshot-v1-with-groups.hex")
        );
        // Busy and idle round-trip although no MVP Host produces them.
        let (_, busy) = decode_snapshot(&fixture("agents-snapshot-v1-busy-idle.hex")).unwrap();
        assert_eq!(busy.agents.unwrap().state, [2, 3, 1]);
        let (_, down) = decode_snapshot(&fixture("agents-snapshot-v1-unavailable.hex")).unwrap();
        let down = down.agents.unwrap();
        assert_eq!(down.availability, Availability::TemporarilyUnavailable);
        assert_eq!(down.state, [0, 0, 0]);
        let max = fixture("agents-snapshot-v1-max.hex");
        assert!(max.len() < 2048);
        let (envelope, snap) = decode_snapshot(&max).unwrap();
        let without = encode_groups(&envelope, &Snapshot::new(snap.cpu.clone())).unwrap();
        // key + map header + five keys + availability + 5-byte age + three states
        assert_eq!(max.len() - without.len(), 16);
        let bad = AgentsSnapshot {
            state: [0, 4, 0],
            ..snap.agents.clone().unwrap()
        };
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    agents: Some(bad.clone()),
                    ..Snapshot::new(snap.cpu.clone())
                }
            ),
            Err(Error::Malformed)
        );
    }

    #[test]
    fn network_group_round_trips_and_stays_within_its_bound() {
        let (envelope, golden) = decode_snapshot(&fixture("network-snapshot-v1.hex")).unwrap();
        let g = golden.network.clone().expect("network group");
        assert_eq!(g.route_kind, RouteKind::Wifi);
        assert!(!g.via_tunnel);
        assert_eq!((g.rx_bytes_per_s, g.tx_bytes_per_s), (1_200_000, 340_000));
        assert_eq!(g.context_availability, AssociationContext::Available);
        assert_eq!((g.coverage, g.channel), (WifiCoverage::Observable, 6));
        assert_eq!(g.station, Some([0xf2, 0xa1, 0xb3, 0xc4, 0xd5, 0xe6]));
        assert_eq!(g.ssid, b"CYD-Test");
        assert_eq!(
            encode_groups(
                &envelope,
                &Snapshot {
                    network: Some(g.clone()),
                    ..Snapshot::new(golden.cpu.clone())
                }
            )
            .unwrap(),
            fixture("network-snapshot-v1.hex")
        );
        let (envelope, all) =
            decode_snapshot(&fixture("network-snapshot-v1-with-groups.hex")).unwrap();
        assert!(all.ram.is_some() && all.agents.is_some() && all.network.is_some());
        assert_eq!(
            encode_groups(&envelope, &all).unwrap(),
            fixture("network-snapshot-v1-with-groups.hex")
        );
        // Every Association Context Availability state round-trips.
        for (i, name) in [
            "available",
            "not-requested",
            "denied",
            "restricted",
            "authorized-unavailable",
            "unsupported",
            "error",
        ]
        .iter()
        .enumerate()
        {
            let bytes = fixture(&format!("network-snapshot-v1-ctx-{name}.hex"));
            let (e, s) = decode_snapshot(&bytes).unwrap();
            let n = s.network.unwrap();
            assert_eq!(n.context_availability as usize, i, "{name}");
            assert_eq!(
                encode_groups(
                    &e,
                    &Snapshot {
                        network: Some(n.clone()),
                        ..Snapshot::new(s.cpu.clone())
                    }
                )
                .unwrap(),
                bytes
            );
        }
        let (_, tunnel) = decode_snapshot(&fixture("network-snapshot-v1-tunnel.hex")).unwrap();
        assert!(tunnel.network.unwrap().via_tunnel);
        let (_, outside) =
            decode_snapshot(&fixture("network-snapshot-v1-5ghz-outside.hex")).unwrap();
        assert_eq!(
            outside.network.unwrap().coverage,
            WifiCoverage::OutsideSupported
        );
        // The largest group: 64-bit rates, 32-bit ages, a 32-byte SSID.
        let max = fixture("network-snapshot-v1-max.hex");
        assert!(max.len() < 2048);
        let (envelope, snap) = decode_snapshot(&max).unwrap();
        let without = encode_groups(&envelope, &Snapshot::new(snap.cpu.clone())).unwrap();
        assert_eq!(max.len() - without.len(), 98);
        let n = snap.network.clone().unwrap();
        for bad in [
            NetworkSnapshot {
                ssid: vec![0; 33],
                ..n.clone()
            },
            NetworkSnapshot {
                context_availability: AssociationContext::Denied,
                ..n.clone()
            },
            NetworkSnapshot {
                traffic_availability: Availability::Error,
                ..n.clone()
            },
            NetworkSnapshot {
                coverage: WifiCoverage::Unknown,
                ..n.clone()
            },
            NetworkSnapshot {
                route_kind: RouteKind::None,
                ..n.clone()
            },
        ] {
            assert_eq!(
                encode_groups(
                    &envelope,
                    &Snapshot {
                        network: Some(bad.clone()),
                        ..Snapshot::new(snap.cpu.clone())
                    }
                ),
                Err(Error::Malformed)
            );
        }
    }
}
