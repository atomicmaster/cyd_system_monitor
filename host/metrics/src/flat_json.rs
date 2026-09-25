// SPDX-License-Identifier: GPL-3.0-only

//! The bounded reader for the one-object-per-line JSON of the daemon's local
//! sockets (the WiFi context provider socket and the control socket). It is
//! deliberately stricter than JSON: one flat object of strings without
//! escapes, canonical integers, booleans and null; nothing nested, no floats,
//! no duplicate keys, and every size bounded. (The battery condition source,
//! `system_profiler` output, is ordinary nested JSON and has its own
//! depth- and node-bounded reader in [`crate::battery`].)

/// Longest accepted line, newline excluded.
pub const MAX_LINE_BYTES: usize = 512;
const MAX_FIELDS: usize = 16;
const MAX_KEY_BYTES: usize = 24;
const MAX_STRING_BYTES: usize = 128;

/// Why a line is not a flat object.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlatJsonError {
    TooLong,
    NotJson,
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
pub fn parse_flat_object(line: &[u8]) -> Result<Vec<(String, Value)>, FlatJsonError> {
    if line.len() > MAX_LINE_BYTES {
        return Err(FlatJsonError::TooLong);
    }
    let text = std::str::from_utf8(line).map_err(|_| FlatJsonError::NotJson)?;
    let b = text.as_bytes();
    let mut p = 0;
    let skip_ws = |p: &mut usize| {
        while *p < b.len() && b[*p] == b' ' {
            *p += 1;
        }
    };
    let string = |p: &mut usize, max: usize| -> Result<String, FlatJsonError> {
        if b.get(*p) != Some(&b'"') {
            return Err(FlatJsonError::NotJson);
        }
        *p += 1;
        let start = *p;
        while *p < b.len() && b[*p] != b'"' {
            // No escapes and no control characters: nothing to decode.
            if b[*p] == b'\\' || b[*p] < 0x20 || *p - start > max {
                return Err(FlatJsonError::NotJson);
            }
            *p += 1;
        }
        if *p >= b.len() {
            return Err(FlatJsonError::NotJson);
        }
        let s = text[start..*p].to_string();
        *p += 1;
        Ok(s)
    };
    skip_ws(&mut p);
    if b.get(p) != Some(&b'{') {
        return Err(FlatJsonError::NotJson);
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
                return Err(FlatJsonError::NotJson);
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
                        return Err(FlatJsonError::NotJson);
                    }
                    Value::Int(digits.parse().map_err(|_| FlatJsonError::NotJson)?)
                }
                _ => return Err(FlatJsonError::NotJson),
            };
            if fields.iter().any(|(k, _)| *k == key) || fields.len() >= MAX_FIELDS {
                return Err(FlatJsonError::NotJson);
            }
            fields.push((key, value));
            skip_ws(&mut p);
            match b.get(p) {
                Some(b',') => p += 1,
                Some(b'}') => {
                    p += 1;
                    break;
                }
                _ => return Err(FlatJsonError::NotJson),
            }
        }
    }
    skip_ws(&mut p);
    if p != b.len() {
        return Err(FlatJsonError::NotJson);
    }
    Ok(fields)
}
