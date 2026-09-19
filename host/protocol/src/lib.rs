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
    let mut c =
        vec![0xa6 + envelope.utc_ms.is_some() as u8 + envelope.utc_uncertainty_ms.is_some() as u8];
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
    c.push(0xa5);
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
    if let Some(utc_ms) = envelope.utc_ms {
        uint(&mut c, 0, 6);
        uint(&mut c, 0, utc_ms);
    }
    if let Some(utc_uncertainty_ms) = envelope.utc_uncertainty_ms {
        uint(&mut c, 0, 7);
        uint(&mut c, 0, utc_uncertainty_ms as u64);
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
pub fn decode_cpu(f: &[u8]) -> Result<(Envelope, CpuSnapshot), Error> {
    if f.len() < 7 {
        return Err(Error::Truncated);
    }
    let n = u16::from_be_bytes([f[0], f[1]]) as usize;
    if n > MAX_FRAME_BYTES - 2 {
        return Err(Error::TooLarge);
    }
    if n + 2 != f.len() {
        return Err(Error::Truncated);
    }
    let c = &f[2..f.len() - 4];
    if crc32(c) != u32::from_be_bytes(f[f.len() - 4..].try_into().unwrap()) {
        return Err(Error::BadCrc);
    }
    let mut p = 0;
    let count = read_uint(c, &mut p, 5).ok_or(Error::Malformed)?;
    if count > 16 {
        return Err(Error::Malformed);
    }
    let (
        mut major,
        mut minor,
        mut kind,
        mut session,
        mut sequence,
        mut utc_ms,
        mut utc_uncertainty_ms,
    ) = (None, None, None, None, None, None, None);
    let mut cpu = [None; 5];
    let mut previous_key = None;
    for _ in 0..count {
        let k = read_uint(c, &mut p, 0).ok_or(Error::Malformed)?;
        // Canonical maps have strictly increasing keys, so a repeat or a
        // reordering is malformed.
        if previous_key.is_some_and(|previous| k <= previous) {
            return Err(Error::Malformed);
        }
        previous_key = Some(k);
        match k {
            0 => {
                major = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            1 => {
                minor = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            2 => {
                kind = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            3 => {
                let len = read_uint(c, &mut p, 2).ok_or(Error::Malformed)?;
                if len != 16 || p + 16 > c.len() {
                    return Err(Error::Malformed);
                }
                session = Some(c[p..p + 16].try_into().unwrap());
                p += 16
            }
            4 => {
                sequence = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            5 => {
                let field_count = read_uint(c, &mut p, 5).ok_or(Error::Malformed)?;
                if field_count > 8 {
                    return Err(Error::Malformed);
                }
                for _ in 0..field_count {
                    let q = read_uint(c, &mut p, 0).ok_or(Error::Malformed)? as usize;
                    // Extension fields in this first body are scalar optional
                    // values; consumers which do not know them retain no data.
                    if q > 4 {
                        read_uint(c, &mut p, 0).ok_or(Error::Malformed)?;
                        continue;
                    }
                    if cpu[q].is_some() {
                        return Err(Error::Malformed);
                    }
                    let value = read_uint(c, &mut p, 0).ok_or(Error::Malformed)?;
                    if ((q == 0 || q == 3) && value > u8::MAX.into())
                        || (q == 1 && value > 1000)
                        || ((q == 2 || q == 4) && value > u32::MAX.into())
                    {
                        return Err(Error::Malformed);
                    }
                    cpu[q] = Some(value)
                }
            }
            6 => utc_ms = Some(read_uint(c, &mut p, 0).ok_or(Error::Malformed)?),
            7 => {
                utc_uncertainty_ms = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            _ => skip_value(c, &mut p, 0)?,
        }
    }
    if p != c.len() || major != Some(1) {
        return Err(if major.is_some() {
            Error::UnsupportedMajor
        } else {
            Error::Malformed
        });
    }
    let kind = match kind.ok_or(Error::Malformed)? {
        1 => MessageType::Hello,
        2 => MessageType::HelloReply,
        3 => MessageType::Pair,
        4 => MessageType::Snapshot,
        5 => MessageType::Command,
        6 => MessageType::CommandReply,
        7 => MessageType::Error,
        _ => return Err(Error::Malformed),
    };
    let availability = match cpu[0].ok_or(Error::Malformed)? {
        0 => Availability::Available,
        1 => Availability::Unsupported,
        2 => Availability::Absent,
        3 => Availability::TemporarilyUnavailable,
        4 => Availability::Error,
        _ => return Err(Error::Malformed),
    };
    let freshness = match cpu[3].ok_or(Error::Malformed)? {
        0 => Freshness::Fresh,
        1 => Freshness::Stale,
        _ => return Err(Error::Malformed),
    };
    Ok((
        Envelope {
            major: 1,
            minor: minor.ok_or(Error::Malformed)?,
            kind,
            session: session.ok_or(Error::Malformed)?,
            sequence: sequence.ok_or(Error::Malformed)?,
            utc_ms,
            utc_uncertainty_ms,
        },
        CpuSnapshot {
            availability,
            utilization_per_mille: cpu[1]
                .ok_or(Error::Malformed)?
                .try_into()
                .map_err(|_| Error::Malformed)?,
            source_age_ms: cpu[2]
                .ok_or(Error::Malformed)?
                .try_into()
                .map_err(|_| Error::Malformed)?,
            freshness,
            configuration_revision: cpu[4]
                .ok_or(Error::Malformed)?
                .try_into()
                .map_err(|_| Error::Malformed)?,
        },
    ))
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
    let mut c =
        vec![0xa6 + envelope.utc_ms.is_some() as u8 + envelope.utc_uncertainty_ms.is_some() as u8];
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

/// Decodes a non-snapshot control frame, including its typed body.
pub fn decode_control(frame: &[u8]) -> Result<(Envelope, ControlBody), Error> {
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
    let (
        mut major,
        mut minor,
        mut kind,
        mut session,
        mut sequence,
        mut utc_ms,
        mut uncertainty,
        mut body,
    ) = (None, None, None, None, None, None, None, None);
    let mut prior = None;
    for _ in 0..count {
        let key = read_uint(c, &mut p, 0).ok_or(Error::Malformed)?;
        if prior.is_some_and(|old| key <= old) {
            return Err(Error::Malformed);
        }
        prior = Some(key);
        match key {
            0 => {
                major = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            1 => {
                minor = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            2 => {
                kind = Some(message_type(
                    read_uint(c, &mut p, 0).ok_or(Error::Malformed)?,
                )?)
            }
            3 => session = Some(bytes(c, &mut p, 16)?.try_into().unwrap()),
            4 => {
                sequence = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            5 => {
                if body.is_some() {
                    return Err(Error::Malformed);
                }
                let start = p;
                skip_value(c, &mut p, 0)?;
                if p - start > 512 {
                    return Err(Error::TooLarge);
                }
                body = Some(&c[start..p]);
            }
            6 => utc_ms = Some(read_uint(c, &mut p, 0).ok_or(Error::Malformed)?),
            7 => {
                uncertainty = Some(
                    read_uint(c, &mut p, 0)
                        .ok_or(Error::Malformed)?
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                )
            }
            _ => skip_value(c, &mut p, 0)?,
        }
    }
    if p != c.len() {
        return Err(Error::Malformed);
    }
    if major != Some(1) {
        return Err(if major.is_some() {
            Error::UnsupportedMajor
        } else {
            Error::Malformed
        });
    }
    let envelope = Envelope {
        major: 1,
        minor: minor.ok_or(Error::Malformed)?,
        kind: kind.ok_or(Error::Malformed)?,
        session: session.ok_or(Error::Malformed)?,
        sequence: sequence.ok_or(Error::Malformed)?,
        utc_ms,
        utc_uncertainty_ms: uncertainty,
    };
    let body = decode_control_body(envelope.kind, body.ok_or(Error::Malformed)?)?;
    Ok((envelope, body))
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
    let count = read_uint(c, &mut p, 5).ok_or(Error::Malformed)?;
    if count > 6 {
        return Err(Error::Malformed);
    }
    let mut fields: [Option<u64>; 6] = [None; 6];
    let mut identities: [Option<[u8; 16]>; 2] = [None, None];
    let mut prior = None;
    for _ in 0..count {
        let key = read_uint(c, &mut p, 0).ok_or(Error::Malformed)? as usize;
        if key >= 6 || prior.is_some_and(|old| key <= old) {
            return Err(Error::Malformed);
        }
        prior = Some(key);
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
        // plausible length that waits for up to 2,046 bytes, so recovery is
        // bounded by that much later traffic rather than immediate.
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
}
