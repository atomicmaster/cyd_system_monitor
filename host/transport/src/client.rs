// SPDX-License-Identifier: GPL-3.0-only

//! Host side of the negotiated link (see `protocol/CORE_PROTOCOL.md`,
//! "Link handshake and pairing"). Pure state machine: bytes in, frames and
//! events out, so it is testable without a serial port.

use protocol::{
    decode, encode_control, encode_groups, CommandBody, CommandOperation, CommandResult,
    ControlBody, CpuSnapshot, Envelope, Error, FrameStream, HelloBody, Message, MessageType,
    PairBody, RamSnapshot, StorageSnapshot,
};

const ZERO: [u8; 16] = [0; 16];

/// The device's reason for refusing the Host (one-byte `Error` body).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    Incompatible,
    NotPairedWithYou,
    Declined,
    BadRequest,
    Unknown(u8),
}

impl Refusal {
    fn from_body(body: &[u8]) -> Refusal {
        match body.first() {
            Some(1) => Refusal::Incompatible,
            Some(2) => Refusal::NotPairedWithYou,
            Some(3) => Refusal::Declined,
            Some(4) => Refusal::BadRequest,
            Some(other) => Refusal::Unknown(*other),
            None => Refusal::Unknown(0),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Idle,
    HelloSent,
    /// Pair sent; a first pairing waits here for the touch confirmation.
    PairSent,
    Live,
    Refused(Refusal),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// HelloReply received; carries the device's supported version.
    Negotiated {
        device_major: u8,
        device_minor: u8,
    },
    /// Pair confirmed; snapshots may now be sent. Carries the device identity
    /// to persist for later reconnects.
    Paired {
        device_identity: [u8; 16],
    },
    Refused(Refusal),
    CommandReply {
        command_identity: u32,
        operation: CommandOperation,
        result: CommandResult,
    },
}

#[derive(Debug, Default)]
pub struct Output {
    /// Frames to write to the device, in order.
    pub frames: Vec<Vec<u8>>,
    pub events: Vec<Event>,
}

pub struct HostClient {
    host: [u8; 16],
    known_device: Option<[u8; 16]>,
    code: u32,
    session: [u8; 16],
    phase: Phase,
    stream: FrameStream,
    sequence: u32,
    next_command: u32,
    minor: u8,
    local_offset_minutes: Option<i16>,
}

impl HostClient {
    /// `code` is the six-digit matching code shown to the operator;
    /// `session` is the fresh random identity for this connection.
    pub fn new(
        host: [u8; 16],
        known_device: Option<[u8; 16]>,
        code: u32,
        session: [u8; 16],
    ) -> Self {
        Self {
            host,
            known_device,
            code,
            session,
            phase: Phase::Idle,
            stream: FrameStream::default(),
            sequence: 0,
            next_command: 1,
            minor: 0,
            local_offset_minutes: None,
        }
    }

    /// The Host's local offset from UTC in minutes east, sent in every
    /// envelope from now on so the display can show local time and follow
    /// daylight-saving changes.
    pub fn set_local_offset_minutes(&mut self, offset: Option<i16>) {
        self.local_offset_minutes = offset;
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }

    fn envelope(&mut self, kind: MessageType, session: [u8; 16], sequence: u32) -> Envelope {
        Envelope {
            major: 1,
            minor: self.minor,
            kind,
            session,
            sequence,
            utc_ms: None,
            utc_uncertainty_ms: None,
            local_offset_minutes: self.local_offset_minutes,
        }
    }

    /// The opening `Hello`. `utc_ms` lets the device show wall-clock time.
    pub fn start(&mut self, utc_ms: Option<u64>) -> Result<Vec<u8>, Error> {
        self.phase = Phase::HelloSent;
        let envelope = self.envelope(MessageType::Hello, ZERO, 1);
        encode_control(
            &envelope,
            &ControlBody::Hello(HelloBody {
                supported_major: 1,
                supported_minor: 0,
                capabilities: 0,
                configuration_revision: 0,
                utc_ms,
                utc_uncertainty_ms: None,
            }),
        )
    }

    /// Feeds bytes from the device (frames interleaved with log text).
    pub fn receive(&mut self, bytes: &[u8]) -> Output {
        self.stream.push(bytes);
        let mut out = Output::default();
        while let Some(frame) = self.stream.next_frame() {
            if let Ok(message) = decode(&frame) {
                self.handle(message, &mut out);
            }
        }
        out
    }

    fn handle(&mut self, message: Message, out: &mut Output) {
        let Message::Control(envelope, body) = message else {
            return;
        };
        match (self.phase, body) {
            (Phase::HelloSent, ControlBody::HelloReply(reply)) => {
                self.minor = 0;
                out.events.push(Event::Negotiated {
                    device_major: reply.supported_major,
                    device_minor: reply.supported_minor,
                });
                let intended = self.known_device.unwrap_or(ZERO);
                let pair = self.envelope(MessageType::Pair, self.session, 2);
                if let Ok(frame) = encode_control(
                    &pair,
                    &ControlBody::Pair(PairBody {
                        pairing_identity: self.host,
                        intended_peer_identity: intended,
                        matching_code: self.code,
                    }),
                ) {
                    out.frames.push(frame);
                    self.phase = Phase::PairSent;
                }
            }
            (Phase::PairSent, ControlBody::Pair(pair))
                if envelope.session == self.session
                    && pair.intended_peer_identity == self.host
                    && pair.matching_code == self.code =>
            {
                self.phase = Phase::Live;
                self.sequence = 0;
                out.events.push(Event::Paired {
                    device_identity: pair.pairing_identity,
                });
            }
            (Phase::HelloSent | Phase::PairSent, ControlBody::Error(body)) => {
                let refusal = Refusal::from_body(&body);
                self.phase = Phase::Refused(refusal);
                out.events.push(Event::Refused(refusal));
            }
            (Phase::Live, ControlBody::CommandReply(reply)) if envelope.session == self.session => {
                out.events.push(Event::CommandReply {
                    command_identity: reply.command_identity,
                    operation: reply.operation,
                    result: reply.result,
                });
            }
            _ => {}
        }
    }

    fn next_sequence(&mut self) -> u32 {
        self.sequence += 1;
        self.sequence
    }

    /// A complete snapshot: CPU plus each optional metric group. Only valid
    /// once paired.
    pub fn snapshot(
        &mut self,
        cpu: &CpuSnapshot,
        ram: Option<&RamSnapshot>,
        storage: Option<&StorageSnapshot>,
        utc_ms: Option<u64>,
    ) -> Option<Vec<u8>> {
        if self.phase != Phase::Live {
            return None;
        }
        let sequence = self.next_sequence();
        let mut envelope = self.envelope(MessageType::Snapshot, self.session, sequence);
        envelope.utc_ms = utc_ms;
        encode_groups(&envelope, cpu, ram, storage).ok()
    }

    /// A command frame and the identity to match its reply against. Reusing
    /// the returned identity with `retry_command` is an idempotent retry.
    pub fn command(&mut self, operation: CommandOperation) -> Option<(u32, Vec<u8>)> {
        let identity = self.next_command;
        self.next_command += 1;
        self.retry_command(identity, operation)
            .map(|frame| (identity, frame))
    }

    pub fn retry_command(&mut self, identity: u32, operation: CommandOperation) -> Option<Vec<u8>> {
        if self.phase != Phase::Live {
            return None;
        }
        let sequence = self.next_sequence();
        let envelope = self.envelope(MessageType::Command, self.session, sequence);
        encode_control(
            &envelope,
            &ControlBody::Command(CommandBody {
                command_identity: identity,
                operation,
            }),
        )
        .ok()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use protocol::{decode, Availability, Freshness};

    #[test]
    fn the_local_offset_rides_every_envelope_once_set() {
        let mut client = HostClient::new([1; 16], None, 123_456, [2; 16]);
        client.set_local_offset_minutes(Some(-300));
        let hello = client.start(Some(1_700_000_000_000)).unwrap();
        let Ok(Message::Control(envelope, _)) = decode(&hello) else {
            panic!("hello")
        };
        assert_eq!(envelope.local_offset_minutes, Some(-300));

        client.phase = Phase::Live;
        let cpu = CpuSnapshot {
            availability: Availability::Available,
            utilization_per_mille: 1,
            source_age_ms: 0,
            freshness: Freshness::Fresh,
            configuration_revision: 0,
        };
        client.set_local_offset_minutes(Some(60)); // daylight saving changed
        let frame = client.snapshot(&cpu, None, None, Some(1)).unwrap();
        let Ok(Message::Snapshot(envelope, _)) = decode(&frame) else {
            panic!("snapshot")
        };
        assert_eq!(envelope.local_offset_minutes, Some(60));

        client.set_local_offset_minutes(None);
        let frame = client.snapshot(&cpu, None, None, Some(2)).unwrap();
        let Ok(Message::Snapshot(envelope, _)) = decode(&frame) else {
            panic!("snapshot")
        };
        assert_eq!(envelope.local_offset_minutes, None);
    }
}
