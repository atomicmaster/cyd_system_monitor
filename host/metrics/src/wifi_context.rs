// SPDX-License-Identifier: GPL-3.0-only

//! Host WiFi association context (TB05): the provider seam, the bounded
//! provider message format and the tracker that decides what the daemon
//! reports. The wire contract is `docs/wifi-context-provider.md`.
//!
//! The daemon never calls CoreWLAN or CoreLocation. A separate signed process
//! (the SwiftUI setup app or a helper it owns) supplies an
//! [`AssociationContext`] over a same-user local socket; this module holds the
//! provider-independent parts so they can be fixture-tested.

use std::sync::{Arc, Mutex};

/// Association Context Availability (`CONTEXT.md`), in wire order
/// (`CORE_PROTOCOL.md`, Network key 6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ContextAvailability {
    Available = 0,
    PermissionNotRequested = 1,
    Denied = 2,
    Restricted = 3,
    AuthorizedUnavailable = 4,
    Unsupported = 5,
    Error = 6,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WifiBand {
    Ghz2_4,
    Ghz5,
    Ghz6,
}

/// What the provider knows about the current association. Band and channel may
/// be known without SSID/BSSID (they need Location permission on macOS); SSID
/// and BSSID are only ever present when `availability` is `Available`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AssociationContext {
    pub availability: ContextAvailability,
    pub band: Option<WifiBand>,
    pub channel: Option<u16>,
    /// 0-32 raw octets.
    pub ssid: Option<Vec<u8>>,
    pub bssid: Option<[u8; 6]>,
}

impl AssociationContext {
    pub fn unavailable(availability: ContextAvailability) -> Self {
        debug_assert_ne!(availability, ContextAvailability::Available);
        Self {
            availability,
            band: None,
            channel: None,
            ssid: None,
            bssid: None,
        }
    }
}

/// A context with the age of the sample behind it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ContextReading {
    pub context: AssociationContext,
    pub age_ms: u32,
}

/// Where association context comes from. `latest` never blocks and never
/// returns a value it cannot vouch for: a missing or silent provider is an
/// unavailable context with a reason, not the last known values.
pub trait WifiContextProvider {
    fn latest(&mut self, now_ms: u64) -> ContextReading;
}

/// A scripted provider: one reading per call, then the last forever. Used by
/// tests and the Host-side scenario in place of the real socket.
pub struct FixtureProvider {
    script: Vec<ContextReading>,
    next: usize,
}

impl FixtureProvider {
    pub fn new(script: Vec<ContextReading>) -> Self {
        assert!(!script.is_empty(), "a script needs at least one reading");
        Self { script, next: 0 }
    }

    /// No provider is connected: permission not requested, age 0.
    pub fn absent() -> Self {
        Self::new(vec![ContextReading {
            context: AssociationContext::unavailable(ContextAvailability::PermissionNotRequested),
            age_ms: 0,
        }])
    }
}

impl WifiContextProvider for FixtureProvider {
    fn latest(&mut self, _now_ms: u64) -> ContextReading {
        let reading = self.script[self.next.min(self.script.len() - 1)].clone();
        self.next += 1;
        reading
    }
}

/// The macOS Location permission as the provider reports it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PermissionStatus {
    NotRequested,
    Denied,
    Restricted,
    Granted,
    Unsupported,
    Error,
}

/// Longest a provider may stay silent before its context is no longer
/// reported: 15 s is fifteen missed 1 s heartbeats, and it equals the device's
/// own staleness limit for the WiFi context half.
pub const CONTEXT_SILENCE_LIMIT_MS: u64 = 15_000;

/// Decides what the daemon reports from what the provider has said.
///
/// - No provider ever connected: permission not requested, no values.
/// - A context is reported with its true age (provider-side sample age plus
///   time since it arrived) until that age reaches 15 s, connected or not.
/// - Past 15 s the values are dropped and the state is a reason: the last
///   reported permission when it was a refusal (`not requested`, `denied`,
///   `restricted`, `unsupported`), else `error` (the provider is gone or hung).
///   A stale value is never presented as fresh.
/// - A permission change applies at once: it drops SSID/BSSID (they need the
///   permission) but keeps band/channel, which do not.
#[derive(Debug, Default)]
pub struct ContextTracker {
    permission: Option<PermissionStatus>,
    connected: bool,
    sample: Option<(AssociationContext, u64, u32)>,
}

fn availability_for(status: PermissionStatus) -> ContextAvailability {
    match status {
        PermissionStatus::NotRequested => ContextAvailability::PermissionNotRequested,
        PermissionStatus::Denied => ContextAvailability::Denied,
        PermissionStatus::Restricted => ContextAvailability::Restricted,
        PermissionStatus::Granted => ContextAvailability::AuthorizedUnavailable,
        PermissionStatus::Unsupported => ContextAvailability::Unsupported,
        PermissionStatus::Error => ContextAvailability::Error,
    }
}

impl ContextTracker {
    /// The last permission status the provider reported, if any.
    pub fn permission(&self) -> Option<PermissionStatus> {
        self.permission
    }

    /// Whether a provider connection that said `hello` is currently open.
    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn on_connected(&mut self) {
        self.connected = true;
    }

    pub fn on_disconnected(&mut self) {
        self.connected = false;
    }

    pub fn on_permission(&mut self, status: PermissionStatus) {
        self.permission = Some(status);
        if let Some((ctx, _, _)) = &mut self.sample {
            let keep = status == PermissionStatus::Granted
                && ctx.availability == ContextAvailability::Available;
            if !keep {
                ctx.availability = availability_for(status);
                ctx.ssid = None;
                ctx.bssid = None;
            }
        }
    }

    pub fn on_context(&mut self, context: AssociationContext, age_ms: u32, now_ms: u64) {
        self.sample = Some((context, now_ms, age_ms));
    }

    pub fn reading(&self, now_ms: u64) -> ContextReading {
        let now = |availability| ContextReading {
            context: AssociationContext::unavailable(availability),
            age_ms: 0,
        };
        if let Some((context, at_ms, age_ms)) = &self.sample {
            let age = now_ms.saturating_sub(*at_ms) + u64::from(*age_ms);
            if age < CONTEXT_SILENCE_LIMIT_MS {
                return ContextReading {
                    context: context.clone(),
                    age_ms: age as u32,
                };
            }
            return now(match self.permission {
                Some(
                    s @ (PermissionStatus::NotRequested
                    | PermissionStatus::Denied
                    | PermissionStatus::Restricted
                    | PermissionStatus::Unsupported),
                ) => availability_for(s),
                _ => ContextAvailability::Error,
            });
        }
        now(match (self.permission, self.connected) {
            (None, _) => ContextAvailability::PermissionNotRequested,
            (Some(status), true) => availability_for(status),
            (Some(PermissionStatus::Granted), false) => ContextAvailability::Error,
            (Some(status), false) => availability_for(status),
        })
    }
}

/// A provider backed by a tracker that a socket server feeds.
#[derive(Clone)]
pub struct TrackerProvider(pub Arc<Mutex<ContextTracker>>);

impl WifiContextProvider for TrackerProvider {
    fn latest(&mut self, now_ms: u64) -> ContextReading {
        self.0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .reading(now_ms)
    }
}

// ---------------------------------------------------------------------------
// Provider messages: one flat JSON object per line, bounded and defensive.
// ---------------------------------------------------------------------------

/// The protocol version this daemon speaks.
pub const IPC_VERSION: i64 = 1;
/// Longest accepted line, newline excluded.
pub const MAX_LINE_BYTES: usize = 512;
const MAX_FIELDS: usize = 16;
const MAX_KEY_BYTES: usize = 24;
const MAX_STRING_BYTES: usize = 128;
const MAX_TEXT_FIELD: usize = 32;

/// A message from the provider.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProviderMessage {
    Hello {
        provider: String,
        provider_version: String,
    },
    PermissionStatus {
        seq: u64,
        status: PermissionStatus,
    },
    Context {
        seq: u64,
        /// The provider's monotonic clock at sampling; ordering only.
        t_ms: u64,
        /// Age of the underlying sample when the message was sent.
        age_ms: u32,
        context: AssociationContext,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageError {
    TooLong,
    NotJson,
    BadVersion,
    UnknownType,
    MissingField,
    BadField,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    Str(String),
    Int(i64),
    Bool(bool),
    Null,
}

/// Parses `{"key": value, ...}` where values are strings without escapes,
/// integers, booleans or null. Nothing nested, no floats, no duplicate keys.
pub fn parse_flat_object(line: &[u8]) -> Result<Vec<(String, Value)>, MessageError> {
    if line.len() > MAX_LINE_BYTES {
        return Err(MessageError::TooLong);
    }
    let text = std::str::from_utf8(line).map_err(|_| MessageError::NotJson)?;
    let b = text.as_bytes();
    let mut p = 0;
    let skip_ws = |p: &mut usize| {
        while *p < b.len() && b[*p] == b' ' {
            *p += 1;
        }
    };
    let string = |p: &mut usize, max: usize| -> Result<String, MessageError> {
        if b.get(*p) != Some(&b'"') {
            return Err(MessageError::NotJson);
        }
        *p += 1;
        let start = *p;
        while *p < b.len() && b[*p] != b'"' {
            // No escapes and no control characters: nothing to decode.
            if b[*p] == b'\\' || b[*p] < 0x20 || *p - start > max {
                return Err(MessageError::NotJson);
            }
            *p += 1;
        }
        if *p >= b.len() {
            return Err(MessageError::NotJson);
        }
        let s = text[start..*p].to_string();
        *p += 1;
        Ok(s)
    };
    skip_ws(&mut p);
    if b.get(p) != Some(&b'{') {
        return Err(MessageError::NotJson);
    }
    p += 1;
    let mut fields: Vec<(String, Value)> = Vec::new();
    skip_ws(&mut p);
    if b.get(p) == Some(&b'}') {
        p += 1;
    } else {
        loop {
            skip_ws(&mut p);
            let key = string(&mut p, MAX_KEY_BYTES)?;
            skip_ws(&mut p);
            if b.get(p) != Some(&b':') {
                return Err(MessageError::NotJson);
            }
            p += 1;
            skip_ws(&mut p);
            let value = match b.get(p) {
                Some(b'"') => Value::Str(string(&mut p, MAX_STRING_BYTES)?),
                Some(b't') if text[p..].starts_with("true") => {
                    p += 4;
                    Value::Bool(true)
                }
                Some(b'f') if text[p..].starts_with("false") => {
                    p += 5;
                    Value::Bool(false)
                }
                Some(b'n') if text[p..].starts_with("null") => {
                    p += 4;
                    Value::Null
                }
                Some(c) if c.is_ascii_digit() || *c == b'-' => {
                    let start = p;
                    p += 1;
                    while p < b.len() && b[p].is_ascii_digit() {
                        p += 1;
                    }
                    let digits = &text[start..p];
                    // Bounded, canonical integers only: no leading zeros.
                    let magnitude = digits.trim_start_matches('-');
                    if magnitude.is_empty()
                        || magnitude.len() > 15
                        || (magnitude.len() > 1 && magnitude.starts_with('0'))
                    {
                        return Err(MessageError::NotJson);
                    }
                    Value::Int(digits.parse().map_err(|_| MessageError::NotJson)?)
                }
                _ => return Err(MessageError::NotJson),
            };
            if fields.iter().any(|(k, _)| *k == key) || fields.len() >= MAX_FIELDS {
                return Err(MessageError::NotJson);
            }
            fields.push((key, value));
            skip_ws(&mut p);
            match b.get(p) {
                Some(b',') => p += 1,
                Some(b'}') => {
                    p += 1;
                    break;
                }
                _ => return Err(MessageError::NotJson),
            }
        }
    }
    skip_ws(&mut p);
    if p != b.len() {
        return Err(MessageError::NotJson);
    }
    Ok(fields)
}

struct Fields(Vec<(String, Value)>);

impl Fields {
    fn get(&self, key: &str) -> Option<&Value> {
        self.0.iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }
    fn str(&self, key: &str) -> Result<&str, MessageError> {
        match self.get(key) {
            Some(Value::Str(s)) => Ok(s),
            Some(_) => Err(MessageError::BadField),
            None => Err(MessageError::MissingField),
        }
    }
    fn uint(&self, key: &str, max: u64) -> Result<u64, MessageError> {
        match self.get(key) {
            Some(Value::Int(n)) if *n >= 0 && (*n as u64) <= max => Ok(*n as u64),
            Some(_) => Err(MessageError::BadField),
            None => Err(MessageError::MissingField),
        }
    }
    /// `None` for null or absent-as-null; wrong types are errors. A required
    /// key must be present (null is an explicit "unknown").
    fn nullable_str(&self, key: &str) -> Result<Option<&str>, MessageError> {
        match self.get(key) {
            Some(Value::Str(s)) => Ok(Some(s)),
            Some(Value::Null) => Ok(None),
            Some(_) => Err(MessageError::BadField),
            None => Err(MessageError::MissingField),
        }
    }
}

fn label(s: &str) -> Result<String, MessageError> {
    if s.is_empty()
        || s.len() > MAX_TEXT_FIELD
        || !s
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._- ".contains(&c))
    {
        return Err(MessageError::BadField);
    }
    Ok(s.to_string())
}

fn hex_bytes(s: &str, max: usize) -> Result<Vec<u8>, MessageError> {
    if !s.len().is_multiple_of(2) || s.len() > max * 2 || !s.bytes().all(|c| c.is_ascii_hexdigit())
    {
        return Err(MessageError::BadField);
    }
    Ok((0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).expect("checked hex"))
        .collect())
}

fn parse_mac(s: &str) -> Result<[u8; 6], MessageError> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != 6 || parts.iter().any(|p| p.len() != 2) {
        return Err(MessageError::BadField);
    }
    let mut out = [0u8; 6];
    for (o, p) in out.iter_mut().zip(parts) {
        *o = hex_bytes(p, 1)?[0];
    }
    Ok(out)
}

fn context_availability(s: &str) -> Result<ContextAvailability, MessageError> {
    Ok(match s {
        "available" => ContextAvailability::Available,
        "not_requested" => ContextAvailability::PermissionNotRequested,
        "denied" => ContextAvailability::Denied,
        "restricted" => ContextAvailability::Restricted,
        "authorized_unavailable" => ContextAvailability::AuthorizedUnavailable,
        "unsupported" => ContextAvailability::Unsupported,
        "error" => ContextAvailability::Error,
        _ => return Err(MessageError::BadField),
    })
}

fn permission_status(s: &str) -> Result<PermissionStatus, MessageError> {
    Ok(match s {
        "not_requested" => PermissionStatus::NotRequested,
        "denied" => PermissionStatus::Denied,
        "restricted" => PermissionStatus::Restricted,
        "granted" => PermissionStatus::Granted,
        "unsupported" => PermissionStatus::Unsupported,
        "error" => PermissionStatus::Error,
        _ => return Err(MessageError::BadField),
    })
}

/// Parses one provider line (without its newline). Unknown extra keys are
/// ignored; everything the daemon uses is range-checked, and SSID/BSSID are
/// refused unless the availability says `available`.
pub fn parse_provider_message(line: &[u8]) -> Result<ProviderMessage, MessageError> {
    let f = Fields(parse_flat_object(line)?);
    match f.get("v") {
        Some(Value::Int(v)) if *v == IPC_VERSION => {}
        Some(Value::Int(_)) => return Err(MessageError::BadVersion),
        Some(_) => return Err(MessageError::BadField),
        None => return Err(MessageError::MissingField),
    }
    match f.str("type")? {
        "hello" => Ok(ProviderMessage::Hello {
            provider: label(f.str("provider")?)?,
            provider_version: label(f.str("provider_version")?)?,
        }),
        "permission_status" => Ok(ProviderMessage::PermissionStatus {
            seq: f.uint("seq", u32::MAX.into())?,
            status: permission_status(f.str("status")?)?,
        }),
        "context" => {
            let availability = context_availability(f.str("availability")?)?;
            let band = match f.nullable_str("band")? {
                None => None,
                Some("2.4") => Some(WifiBand::Ghz2_4),
                Some("5") => Some(WifiBand::Ghz5),
                Some("6") => Some(WifiBand::Ghz6),
                Some(_) => return Err(MessageError::BadField),
            };
            let channel = match f.get("channel") {
                Some(Value::Null) => None,
                Some(Value::Int(c)) if (1..=233).contains(c) => Some(*c as u16),
                Some(_) => return Err(MessageError::BadField),
                None => return Err(MessageError::MissingField),
            };
            let ssid = f
                .nullable_str("ssid_hex")?
                .map(|s| hex_bytes(s, 32))
                .transpose()?;
            let bssid = f.nullable_str("bssid")?.map(parse_mac).transpose()?;
            if (ssid.is_some() || bssid.is_some()) && availability != ContextAvailability::Available
            {
                return Err(MessageError::BadField);
            }
            Ok(ProviderMessage::Context {
                seq: f.uint("seq", u32::MAX.into())?,
                t_ms: f.uint("t_ms", 1 << 53)?,
                age_ms: f.uint("age_ms", u32::MAX.into())? as u32,
                context: AssociationContext {
                    availability,
                    band,
                    channel,
                    ssid,
                    bssid,
                },
            })
        }
        _ => Err(MessageError::UnknownType),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ctx_line(extra: &str) -> String {
        format!(
            r#"{{"v":1,"type":"context","seq":7,"t_ms":123456,"age_ms":40,"availability":"available","band":"2.4","channel":6,"ssid_hex":"4359442d54657374","bssid":"0a:1b:2c:3d:4e:5f"{extra}}}"#
        )
    }

    #[test]
    fn a_full_context_message_parses() {
        let m = parse_provider_message(ctx_line("").as_bytes()).unwrap();
        let ProviderMessage::Context {
            seq,
            t_ms,
            age_ms,
            context,
        } = m
        else {
            panic!("context")
        };
        assert_eq!((seq, t_ms, age_ms), (7, 123456, 40));
        assert_eq!(context.availability, ContextAvailability::Available);
        assert_eq!(
            (context.band, context.channel),
            (Some(WifiBand::Ghz2_4), Some(6))
        );
        assert_eq!(context.ssid.as_deref(), Some(&b"CYD-Test"[..]));
        assert_eq!(context.bssid, Some([0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f]));
        // Unknown keys are ignored (forward compatibility within version 1).
        assert!(parse_provider_message(ctx_line(r#","future":true"#).as_bytes()).is_ok());
    }

    #[test]
    fn hello_and_permission_messages_parse() {
        let hello =
            r#"{"v":1,"type":"hello","provider":"cyd-setup-app","provider_version":"0.1.0"}"#;
        assert_eq!(
            parse_provider_message(hello.as_bytes()),
            Ok(ProviderMessage::Hello {
                provider: "cyd-setup-app".into(),
                provider_version: "0.1.0".into()
            })
        );
        for (word, status) in [
            ("not_requested", PermissionStatus::NotRequested),
            ("denied", PermissionStatus::Denied),
            ("restricted", PermissionStatus::Restricted),
            ("granted", PermissionStatus::Granted),
            ("unsupported", PermissionStatus::Unsupported),
            ("error", PermissionStatus::Error),
        ] {
            let line = format!(r#"{{"v":1,"type":"permission_status","seq":1,"status":"{word}"}}"#);
            assert_eq!(
                parse_provider_message(line.as_bytes()),
                Ok(ProviderMessage::PermissionStatus { seq: 1, status })
            );
        }
    }

    #[test]
    fn every_association_context_availability_is_accepted() {
        for (word, availability) in [
            ("available", ContextAvailability::Available),
            ("not_requested", ContextAvailability::PermissionNotRequested),
            ("denied", ContextAvailability::Denied),
            ("restricted", ContextAvailability::Restricted),
            (
                "authorized_unavailable",
                ContextAvailability::AuthorizedUnavailable,
            ),
            ("unsupported", ContextAvailability::Unsupported),
            ("error", ContextAvailability::Error),
        ] {
            let line = format!(
                r#"{{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"{word}","band":"5","channel":149,"ssid_hex":null,"bssid":null}}"#
            );
            let Ok(ProviderMessage::Context { context, .. }) =
                parse_provider_message(line.as_bytes())
            else {
                panic!("{word}")
            };
            assert_eq!(context.availability, availability);
        }
    }

    #[test]
    fn malformed_lines_are_rejected_without_panicking() {
        let bad = [
            "",
            "{",
            "[]",
            "null",
            r#"{"v":1,"type":"context""#,
            r#"{"v":1,"type":"context"} trailing"#,
            r#"{"v":2,"type":"hello","provider":"a","provider_version":"1"}"#,
            r#"{"v":"1","type":"hello"}"#,
            r#"{"type":"hello"}"#,
            r#"{"v":1,"type":"launch_missiles"}"#,
            r#"{"v":1,"v":1,"type":"hello","provider":"a","provider_version":"1"}"#,
            r#"{"v":1,"type":"hello","provider":"a\\u0041","provider_version":"1"}"#,
            r#"{"v":1,"type":"hello","provider":"","provider_version":"1"}"#,
            r#"{"v":1,"type":"hello","provider":"a/b","provider_version":"1"}"#,
            r#"{"v":1,"type":"permission_status","seq":-1,"status":"denied"}"#,
            r#"{"v":1,"type":"permission_status","seq":1,"status":"maybe"}"#,
            r#"{"v":1,"type":"permission_status","seq":1.5,"status":"denied"}"#,
            r#"{"v":1,"type":"permission_status","seq":01,"status":"denied"}"#,
            r#"{"v":1,"type":"permission_status","seq":99999999999999999999,"status":"denied"}"#,
            r#"{"v":1,"type":"permission_status","seq":4294967296,"status":"denied"}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":"7","channel":1,"ssid_hex":null,"bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":234,"ssid_hex":null,"bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":0,"ssid_hex":null,"bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":4294967296,"availability":"available","band":null,"channel":null,"ssid_hex":null,"bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":null,"ssid_hex":"abc","bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":null,"ssid_hex":"zz","bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":null,"ssid_hex":null,"bssid":"0a:1b:2c:3d:4e"}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":null,"ssid_hex":null,"bssid":"0a1b2c3d4e5f"}"#,
            // SSID or BSSID while permission says otherwise.
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"denied","band":null,"channel":null,"ssid_hex":"41","bssid":null}"#,
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"authorized_unavailable","band":null,"channel":null,"ssid_hex":null,"bssid":"0a:1b:2c:3d:4e:5f"}"#,
            // Missing required field.
            r#"{"v":1,"type":"context","seq":1,"t_ms":1,"availability":"denied","band":null,"channel":null,"ssid_hex":null,"bssid":null}"#,
            // Nesting is not part of the format.
            r#"{"v":1,"type":"context","x":{"a":1}}"#,
            r#"{"v":1,"type":"context","x":[1]}"#,
        ];
        for line in bad {
            assert!(parse_provider_message(line.as_bytes()).is_err(), "{line}");
        }
        // 33-byte SSID: over the bound.
        let long = format!(
            r#"{{"v":1,"type":"context","seq":1,"t_ms":1,"age_ms":0,"availability":"available","band":null,"channel":null,"ssid_hex":"{}","bssid":null}}"#,
            "41".repeat(33)
        );
        assert_eq!(
            parse_provider_message(long.as_bytes()),
            Err(MessageError::BadField)
        );
        // Oversized line and non-UTF-8 bytes.
        let huge = vec![b' '; MAX_LINE_BYTES + 1];
        assert_eq!(parse_provider_message(&huge), Err(MessageError::TooLong));
        assert_eq!(
            parse_provider_message(&[0xff, 0xfe, b'{']),
            Err(MessageError::NotJson)
        );
    }

    #[test]
    fn arbitrary_bytes_never_panic_the_parser() {
        // A deterministic pseudo-random sweep plus mutations of a valid line.
        let mut state = 0x9E37_79B9_7F4A_7C15u64;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        let valid = ctx_line("").into_bytes();
        for round in 0..20_000 {
            let mut line = if round % 2 == 0 {
                valid.clone()
            } else {
                (0..(next() % 300) as usize).map(|_| next() as u8).collect()
            };
            if !line.is_empty() {
                for _ in 0..(next() % 4) {
                    let i = (next() as usize) % line.len();
                    line[i] = b"{}\":,\\ 09-tfnx\xff"[(next() % 15) as usize];
                }
                let cut = (next() as usize) % (line.len() + 1);
                line.truncate(cut);
            }
            let _ = parse_provider_message(&line);
        }
    }

    fn ctx(availability: ContextAvailability, ssid: bool) -> AssociationContext {
        AssociationContext {
            availability,
            band: Some(WifiBand::Ghz2_4),
            channel: Some(6),
            ssid: ssid.then(|| b"CYD".to_vec()),
            bssid: ssid.then_some([1; 6]),
        }
    }

    #[test]
    fn with_no_provider_context_is_permission_not_requested() {
        let t = ContextTracker::default();
        let r = t.reading(1_000);
        assert_eq!(
            r.context.availability,
            ContextAvailability::PermissionNotRequested
        );
        assert_eq!((r.age_ms, r.context.channel), (0, None));
    }

    #[test]
    fn a_context_is_reported_with_its_true_age_and_expires_after_15_s() {
        let mut t = ContextTracker::default();
        t.on_connected();
        t.on_context(ctx(ContextAvailability::Available, true), 500, 10_000);
        let r = t.reading(12_000);
        assert_eq!(r.age_ms, 2_500);
        assert_eq!(r.context.availability, ContextAvailability::Available);
        assert_eq!(
            t.reading(24_499).context.availability,
            ContextAvailability::Available
        );
        // The provider went quiet: at 15 s the values are gone, with a reason.
        let gone = t.reading(24_500);
        assert_eq!(gone.context.availability, ContextAvailability::Error);
        assert!(gone.context.ssid.is_none() && gone.context.channel.is_none());
        assert_eq!(gone.age_ms, 0);
        // Disconnecting does not extend it either.
        t.on_disconnected();
        assert_eq!(
            t.reading(30_000).context.availability,
            ContextAvailability::Error
        );
    }

    #[test]
    fn an_already_old_sample_expires_by_its_own_age() {
        let mut t = ContextTracker::default();
        t.on_context(ctx(ContextAvailability::Available, true), 14_800, 0);
        assert_eq!(
            t.reading(100).context.availability,
            ContextAvailability::Available
        );
        assert_eq!(
            t.reading(200).context.availability,
            ContextAvailability::Error
        );
    }

    #[test]
    fn a_refusal_stays_a_refusal_after_the_provider_goes_away() {
        let mut t = ContextTracker::default();
        t.on_connected();
        t.on_permission(PermissionStatus::Denied);
        t.on_context(ctx(ContextAvailability::Denied, false), 0, 0);
        t.on_disconnected();
        assert_eq!(
            t.reading(1_000).context.availability,
            ContextAvailability::Denied
        );
        assert_eq!(
            t.reading(60_000).context.availability,
            ContextAvailability::Denied
        );
        // A granted permission with a vanished provider is not "authorized".
        let mut g = ContextTracker::default();
        g.on_connected();
        g.on_permission(PermissionStatus::Granted);
        assert_eq!(
            g.reading(0).context.availability,
            ContextAvailability::AuthorizedUnavailable
        );
        g.on_disconnected();
        assert_eq!(
            g.reading(0).context.availability,
            ContextAvailability::Error
        );
    }

    #[test]
    fn a_permission_change_takes_effect_at_once_and_drops_only_identity() {
        let mut t = ContextTracker::default();
        t.on_connected();
        t.on_context(ctx(ContextAvailability::Available, true), 0, 0);
        t.on_permission(PermissionStatus::Granted); // still available
        assert_eq!(t.reading(10).context.ssid.as_deref(), Some(&b"CYD"[..]));
        t.on_permission(PermissionStatus::Denied); // revoked in System Settings
        let r = t.reading(20).context;
        assert_eq!(r.availability, ContextAvailability::Denied);
        assert!(r.ssid.is_none() && r.bssid.is_none());
        assert_eq!((r.band, r.channel), (Some(WifiBand::Ghz2_4), Some(6)));
    }

    #[test]
    fn fixture_provider_replays_its_script_then_repeats_the_last() {
        let a = ContextReading {
            context: ctx(ContextAvailability::Available, true),
            age_ms: 1,
        };
        let b = ContextReading {
            context: AssociationContext::unavailable(ContextAvailability::Denied),
            age_ms: 2,
        };
        let mut p = FixtureProvider::new(vec![a.clone(), b.clone()]);
        assert_eq!(p.latest(0), a);
        assert_eq!(p.latest(1), b);
        assert_eq!(p.latest(2), b);
        assert_eq!(
            FixtureProvider::absent().latest(0).context.availability,
            ContextAvailability::PermissionNotRequested
        );
    }
}
