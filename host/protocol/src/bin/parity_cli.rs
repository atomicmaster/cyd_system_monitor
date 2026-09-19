// SPDX-License-Identifier: GPL-3.0-only
//! Differential-parity harness, Rust side. Prints the same canonical outcome
//! lines as protocol/tests/cpp/parity_cli.cpp for `F`, `S` and `B` input lines;
//! protocol/tools/parity_diff.py compares the two.
use protocol::*;
use std::io::BufRead;

fn hex(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "-".into();
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn opt<T: ToString>(value: Option<T>) -> String {
    value.map_or("-".into(), |v| v.to_string())
}
fn error_name(error: Error) -> &'static str {
    match error {
        Error::Truncated => "truncated",
        Error::TooLarge => "too-large",
        Error::BadCrc => "bad-crc",
        Error::Malformed => "malformed",
        Error::UnsupportedMajor => "unsupported-major",
        Error::WrongSession => "wrong-session",
        Error::StaleSequence => "stale-sequence",
    }
}
fn kind_name(kind: MessageType) -> &'static str {
    match kind {
        MessageType::Hello => "hello",
        MessageType::HelloReply => "hello-reply",
        MessageType::Pair => "pair",
        MessageType::Snapshot => "snapshot",
        MessageType::Command => "command",
        MessageType::CommandReply => "command-reply",
        MessageType::Error => "error",
    }
}
fn from_hex(text: &str) -> Vec<u8> {
    (0..text.len() / 2)
        .map(|i| u8::from_str_radix(&text[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

type Decoded = Message;

fn decode_frame(frame: &[u8]) -> Result<Decoded, String> {
    decode(frame).map_err(|error| error_name(error).to_string())
}

fn summary(result: &Result<Decoded, String>) -> String {
    let (envelope, body) = match result {
        Err(name) => return format!("err {name}"),
        Ok(Message::Snapshot(e, snapshot)) => {
            let c = &snapshot.cpu;
            let mut text = format!(
                "{},{},{},{},{}",
                c.availability as u8,
                c.utilization_per_mille,
                c.source_age_ms,
                c.freshness as u8,
                c.configuration_revision
            );
            if let Some(r) = &snapshot.ram {
                text += &format!(
                    ";ram={},{},{},{},{}",
                    r.availability as u8,
                    r.used_bytes,
                    r.total_bytes,
                    r.source_age_ms,
                    r.freshness as u8
                );
            }
            (e, text)
        }
        Ok(Message::Control(e, body)) => (
            e,
            match body {
                ControlBody::Hello(h) | ControlBody::HelloReply(h) => format!(
                    "{},{},{},{},{},{}",
                    h.supported_major,
                    h.supported_minor,
                    h.capabilities,
                    h.configuration_revision,
                    opt(h.utc_ms),
                    opt(h.utc_uncertainty_ms)
                ),
                ControlBody::Pair(p) => format!(
                    "{},{},{}",
                    hex(&p.pairing_identity),
                    hex(&p.intended_peer_identity),
                    p.matching_code
                ),
                ControlBody::Command(c) => {
                    format!("{},{},-", c.command_identity, c.operation as u8)
                }
                ControlBody::CommandReply(c) => format!(
                    "{},{},{}",
                    c.command_identity, c.operation as u8, c.result as u8
                ),
                ControlBody::Error(bytes) => hex(bytes),
            },
        ),
    };
    format!(
        "ok {} v{}.{} sess={} seq={} utc={} unc={} off={} body={}",
        kind_name(envelope.kind),
        envelope.major,
        envelope.minor,
        hex(&envelope.session),
        envelope.sequence,
        opt(envelope.utc_ms),
        opt(envelope.utc_uncertainty_ms),
        opt(envelope.local_offset_minutes),
        body
    )
}

fn reencode(decoded: &Decoded) -> String {
    let result = match decoded {
        Message::Snapshot(e, s) => encode_snapshot(e, &s.cpu, s.ram.as_ref()),
        Message::Control(e, b) => encode_control(e, b),
    };
    match result {
        Ok(frame) => hex(&frame),
        Err(error) => format!("err {}", error_name(error)),
    }
}

fn run_stream(bytes: &[u8], bytewise: bool) -> String {
    let mut stream = FrameStream::default();
    let mut out = String::new();
    let drain = |stream: &mut FrameStream, out: &mut String| {
        while let Some(frame) = stream.next_frame() {
            out.push_str(&summary(&decode_frame(&frame)));
            out.push(';');
        }
    };
    if bytewise {
        for byte in bytes {
            stream.push(&[*byte]);
            drain(&mut stream, &mut out);
        }
    } else {
        stream.push(bytes);
        drain(&mut stream, &mut out);
    }
    while stream.buffered_bytes() > 0 {
        stream.drop_leading_byte();
        drain(&mut stream, &mut out);
    }
    format!("{out} dropped={}", stream.dropped_bytes())
}

fn main() {
    for line in std::io::stdin().lock().lines() {
        let line = line.unwrap();
        let bytes = from_hex(line.get(2..).unwrap_or(""));
        if line.starts_with('F') {
            let decoded = decode_frame(&bytes);
            let mut out = summary(&decoded);
            if let Ok(value) = &decoded {
                out.push_str(&format!(" re={}", reencode(value)));
            }
            println!("{out}");
        } else {
            println!("{}", run_stream(&bytes, line.starts_with('B')));
        }
    }
}
