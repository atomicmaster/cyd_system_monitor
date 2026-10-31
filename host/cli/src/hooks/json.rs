// SPDX-License-Identifier: GPL-3.0-only

//! A small strict JSON reader and writer for the agents' hook configs. Objects
//! keep their key order and numbers keep their source text, so a file we edit
//! changes only where we mean it to. The reader follows RFC 8259 and nothing
//! more: no comments, no trailing commas, no duplicate keys, no byte order
//! mark, bounded size and nesting. Anything else is an error, and the caller
//! reports "config unreadable" instead of rewriting the file.

/// Largest document read (bytes). Hook configs are a few kilobytes.
pub const MAX_BYTES: usize = 1 << 20;
const MAX_DEPTH: usize = 32;

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    /// The number exactly as written.
    Num(String),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn str(text: &str) -> Json {
        Json::Str(text.to_string())
    }

    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(fields) => fields.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut Json> {
        match self {
            Json::Obj(fields) => fields.iter_mut().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(text) => Some(text),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&Vec<Json>> {
        match self {
            Json::Arr(items) => Some(items),
            _ => None,
        }
    }

    /// Sets `key`, replacing the value in place or appending a new field.
    /// Only meaningful on an object.
    pub fn set(&mut self, key: &str, value: Json) {
        if let Json::Obj(fields) = self {
            match fields.iter_mut().find(|(k, _)| k == key) {
                Some((_, slot)) => *slot = value,
                None => fields.push((key.to_string(), value)),
            }
        }
    }

    /// Removes `key` from an object; true when it was there.
    pub fn remove(&mut self, key: &str) -> bool {
        match self {
            Json::Obj(fields) => {
                let before = fields.len();
                fields.retain(|(k, _)| k != key);
                fields.len() != before
            }
            _ => false,
        }
    }

    pub fn is_empty_object(&self) -> bool {
        matches!(self, Json::Obj(fields) if fields.is_empty())
    }
}

/// Why a document was refused.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JsonError(pub String);

impl std::fmt::Display for JsonError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

fn fail<T>(at: usize, what: &str) -> Result<T, JsonError> {
    Err(JsonError(format!("{what} at byte {at}")))
}

/// Parses one JSON document of at most [`MAX_BYTES`].
pub fn parse(text: &str) -> Result<Json, JsonError> {
    parse_limited(text, MAX_BYTES)
}

pub fn parse_limited(text: &str, max_bytes: usize) -> Result<Json, JsonError> {
    if text.len() > max_bytes {
        return Err(JsonError(format!("larger than {max_bytes} bytes")));
    }
    let mut parser = Parser {
        b: text.as_bytes(),
        text,
        p: 0,
    };
    parser.skip_ws();
    let value = parser.value(0)?;
    parser.skip_ws();
    if parser.p != parser.b.len() {
        return fail(parser.p, "unexpected data after the value");
    }
    Ok(value)
}

struct Parser<'a> {
    b: &'a [u8],
    text: &'a str,
    p: usize,
}

impl Parser<'_> {
    fn skip_ws(&mut self) {
        while matches!(self.b.get(self.p), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.p += 1;
        }
    }

    fn eat(&mut self, literal: &str) -> bool {
        if self.b[self.p..].starts_with(literal.as_bytes()) {
            self.p += literal.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self, depth: usize) -> Result<Json, JsonError> {
        if depth >= MAX_DEPTH {
            return fail(self.p, "nesting too deep");
        }
        match self.b.get(self.p) {
            Some(b'{') => self.object(depth),
            Some(b'[') => self.array(depth),
            Some(b'"') => self.string().map(Json::Str),
            Some(b't') if self.eat("true") => Ok(Json::Bool(true)),
            Some(b'f') if self.eat("false") => Ok(Json::Bool(false)),
            Some(b'n') if self.eat("null") => Ok(Json::Null),
            Some(b'-' | b'0'..=b'9') => self.number(),
            Some(b'/') => fail(self.p, "comments are not allowed"),
            Some(_) => fail(self.p, "unexpected character"),
            None => fail(self.p, "unexpected end"),
        }
    }

    fn bad_delimiter<T>(&self, close: &str) -> Result<T, JsonError> {
        match self.b.get(self.p) {
            Some(b'/') => fail(self.p, "comments are not allowed"),
            _ => fail(self.p, &format!("expected ',' or {close}")),
        }
    }

    fn object(&mut self, depth: usize) -> Result<Json, JsonError> {
        self.p += 1;
        let mut fields: Vec<(String, Json)> = Vec::new();
        self.skip_ws();
        if self.b.get(self.p) == Some(&b'}') {
            self.p += 1;
            return Ok(Json::Obj(fields));
        }
        loop {
            self.skip_ws();
            if self.b.get(self.p) != Some(&b'"') {
                if self.b.get(self.p) == Some(&b'/') {
                    return fail(self.p, "comments are not allowed");
                }
                return fail(self.p, "expected a string key");
            }
            let key = self.string()?;
            if fields.iter().any(|(k, _)| *k == key) {
                return fail(self.p, &format!("duplicate key {key:?}"));
            }
            self.skip_ws();
            if self.b.get(self.p) != Some(&b':') {
                return fail(self.p, "expected ':'");
            }
            self.p += 1;
            self.skip_ws();
            let value = self.value(depth + 1)?;
            fields.push((key, value));
            self.skip_ws();
            match self.b.get(self.p) {
                Some(b',') => self.p += 1,
                Some(b'}') => {
                    self.p += 1;
                    return Ok(Json::Obj(fields));
                }
                _ => return self.bad_delimiter("'}'"),
            }
        }
    }

    fn array(&mut self, depth: usize) -> Result<Json, JsonError> {
        self.p += 1;
        let mut items = Vec::new();
        self.skip_ws();
        if self.b.get(self.p) == Some(&b']') {
            self.p += 1;
            return Ok(Json::Arr(items));
        }
        loop {
            self.skip_ws();
            items.push(self.value(depth + 1)?);
            self.skip_ws();
            match self.b.get(self.p) {
                Some(b',') => self.p += 1,
                Some(b']') => {
                    self.p += 1;
                    return Ok(Json::Arr(items));
                }
                _ => return self.bad_delimiter("']'"),
            }
        }
    }

    fn hex4(&mut self) -> Result<u32, JsonError> {
        let digits = self.text.get(self.p..self.p + 4);
        match digits.filter(|d| d.bytes().all(|c| c.is_ascii_hexdigit())) {
            Some(d) => {
                self.p += 4;
                Ok(u32::from_str_radix(d, 16).unwrap_or(0))
            }
            None => fail(self.p, "bad \\u escape"),
        }
    }

    fn string(&mut self) -> Result<String, JsonError> {
        self.p += 1;
        let mut out = String::new();
        loop {
            let Some(&c) = self.b.get(self.p) else {
                return fail(self.p, "unterminated string");
            };
            match c {
                b'"' => {
                    self.p += 1;
                    return Ok(out);
                }
                b'\\' => {
                    self.p += 1;
                    let Some(&esc) = self.b.get(self.p) else {
                        return fail(self.p, "unterminated string");
                    };
                    self.p += 1;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let first = self.hex4()?;
                            let code = if (0xD800..0xDC00).contains(&first) {
                                if !self.eat("\\u") {
                                    return fail(self.p, "lone surrogate");
                                }
                                let second = self.hex4()?;
                                if !(0xDC00..0xE000).contains(&second) {
                                    return fail(self.p, "lone surrogate");
                                }
                                0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00)
                            } else {
                                first
                            };
                            match char::from_u32(code) {
                                Some(ch) => out.push(ch),
                                None => return fail(self.p, "lone surrogate"),
                            }
                        }
                        _ => return fail(self.p - 1, "bad escape"),
                    }
                }
                0..=0x1f => return fail(self.p, "control character in string"),
                _ => {
                    // The input is a &str, so this is the start of a whole
                    // character; copy it and step over its bytes.
                    let ch = self.text[self.p..].chars().next().unwrap_or('\u{fffd}');
                    out.push(ch);
                    self.p += ch.len_utf8();
                }
            }
        }
    }

    fn number(&mut self) -> Result<Json, JsonError> {
        let start = self.p;
        let digits = |s: &mut Self| {
            let from = s.p;
            while matches!(s.b.get(s.p), Some(b'0'..=b'9')) {
                s.p += 1;
            }
            s.p - from
        };
        if self.b.get(self.p) == Some(&b'-') {
            self.p += 1;
        }
        match self.b.get(self.p) {
            Some(b'0') => self.p += 1,
            Some(b'1'..=b'9') => {
                digits(self);
            }
            _ => return fail(self.p, "bad number"),
        }
        if self.b.get(self.p) == Some(&b'.') {
            self.p += 1;
            if digits(self) == 0 {
                return fail(self.p, "bad number");
            }
        }
        if matches!(self.b.get(self.p), Some(b'e' | b'E')) {
            self.p += 1;
            if matches!(self.b.get(self.p), Some(b'+' | b'-')) {
                self.p += 1;
            }
            if digits(self) == 0 {
                return fail(self.p, "bad number");
            }
        }
        Ok(Json::Num(self.text[start..self.p].to_string()))
    }
}

/// Writes `value` with two-space indentation and a final newline. Empty
/// arrays and objects stay on one line. Non-ASCII text is written as is.
pub fn pretty(value: &Json) -> String {
    let mut out = String::new();
    write_value(&mut out, value, 0);
    out.push('\n');
    out
}

fn indent(out: &mut String, depth: usize) {
    for _ in 0..depth {
        out.push_str("  ");
    }
}

fn write_value(out: &mut String, value: &Json, depth: usize) {
    match value {
        Json::Null => out.push_str("null"),
        Json::Bool(true) => out.push_str("true"),
        Json::Bool(false) => out.push_str("false"),
        Json::Num(text) => out.push_str(text),
        Json::Str(text) => write_string(out, text),
        Json::Arr(items) if items.is_empty() => out.push_str("[]"),
        Json::Arr(items) => {
            out.push_str("[\n");
            for (i, item) in items.iter().enumerate() {
                indent(out, depth + 1);
                write_value(out, item, depth + 1);
                out.push_str(if i + 1 < items.len() { ",\n" } else { "\n" });
            }
            indent(out, depth);
            out.push(']');
        }
        Json::Obj(fields) if fields.is_empty() => out.push_str("{}"),
        Json::Obj(fields) => {
            out.push_str("{\n");
            for (i, (key, item)) in fields.iter().enumerate() {
                indent(out, depth + 1);
                write_string(out, key);
                out.push_str(": ");
                write_value(out, item, depth + 1);
                out.push_str(if i + 1 < fields.len() { ",\n" } else { "\n" });
            }
            indent(out, depth);
            out.push('}');
        }
    }
}

/// Compact form, for matching a value's text (no indentation, no newline).
pub fn compact(value: &Json) -> String {
    let mut out = String::new();
    write_compact(&mut out, value);
    out
}

fn write_compact(out: &mut String, value: &Json) {
    match value {
        Json::Arr(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_compact(out, item);
            }
            out.push(']');
        }
        Json::Obj(fields) => {
            out.push('{');
            for (i, (key, item)) in fields.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_string(out, key);
                out.push(':');
                write_compact(out, item);
            }
            out.push('}');
        }
        other => write_value(out, other, 0),
    }
}

fn write_string(out: &mut String, text: &str) {
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ok(text: &str) -> Json {
        parse(text).unwrap_or_else(|e| panic!("{text:?}: {e}"))
    }

    #[test]
    fn scalars_and_containers_parse() {
        assert_eq!(ok(" null "), Json::Null);
        assert_eq!(ok("true"), Json::Bool(true));
        assert_eq!(ok("false"), Json::Bool(false));
        assert_eq!(ok("\"a\""), Json::str("a"));
        assert_eq!(ok("[]"), Json::Arr(vec![]));
        assert_eq!(ok("{}"), Json::Obj(vec![]));
        assert_eq!(
            ok("{\"a\": [1, {\"b\": null}]}"),
            Json::Obj(vec![(
                "a".into(),
                Json::Arr(vec![
                    Json::Num("1".into()),
                    Json::Obj(vec![("b".into(), Json::Null)])
                ])
            )])
        );
    }

    #[test]
    fn object_key_order_is_kept() {
        let text = "{\"z\": 1, \"a\": 2, \"m\": 3}";
        let Json::Obj(fields) = ok(text) else {
            panic!("not an object")
        };
        let keys: Vec<&str> = fields.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, ["z", "a", "m"]);
        assert_eq!(
            pretty(&ok(text)),
            "{\n  \"z\": 1,\n  \"a\": 2,\n  \"m\": 3\n}\n"
        );
    }

    #[test]
    fn numbers_keep_their_source_text() {
        for number in [
            "0",
            "-0",
            "10",
            "1.50",
            "-2.5e+10",
            "1E5",
            "0.000",
            "123456789012345678901234567890",
        ] {
            assert_eq!(ok(number), Json::Num(number.into()));
            assert_eq!(pretty(&ok(number)), format!("{number}\n"));
        }
        for bad in [
            "01", "1.", ".5", "+1", "-", "1e", "1e+", "0x10", "NaN", "Infinity",
        ] {
            assert!(parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn string_escapes_and_unicode_decode() {
        assert_eq!(
            ok(r#""a\"b\\c\/d\b\f\n\r\t""#),
            Json::str("a\"b\\c/d\u{8}\u{c}\n\r\t")
        );
        assert_eq!(ok(r#""éA""#), Json::str("éA"));
        assert_eq!(ok(r#""😀""#), Json::str("😀"));
        assert_eq!(ok("\"héllo 😀\""), Json::str("héllo 😀"));
        for bad in [
            r#""\ud83d""#,
            r#""\ude00""#,
            r#""\ud83dA""#,
            r#""\u12""#,
            r#""\x""#,
            "\"a\nb\"",
            "\"a\u{1}b\"",
            "\"open",
            "\"\\",
        ] {
            assert!(parse(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn writer_escapes_and_round_trips_text() {
        let value = Json::str("q\" b\\ n\n t\t c\u{1} é 😀");
        let text = pretty(&value);
        assert_eq!(text, "\"q\\\" b\\\\ n\\n t\\t c\\u0001 é 😀\"\n");
        assert_eq!(ok(&text), value);
    }

    #[test]
    fn pretty_output_is_stable_and_reparses_equal() {
        let text = "{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"timeout\":5}]}],\"Empty\":[],\"E\":{}},\"n\":-1.5e3,\"s\":\"é\",\"t\":true,\"z\":null}";
        let value = ok(text);
        let once = pretty(&value);
        assert_eq!(ok(&once), value);
        assert_eq!(pretty(&ok(&once)), once);
        assert!(once.contains("    \"Stop\": [\n      {\n        \"hooks\": [\n"));
        assert!(once.contains("\"Empty\": []"));
        assert!(once.contains("\"E\": {}"));
        assert!(once.ends_with("}\n"));
    }

    #[test]
    fn compact_has_no_whitespace() {
        assert_eq!(
            compact(&ok("{ \"a\" : [ 1 , \"x y\" ] }")),
            "{\"a\":[1,\"x y\"]}"
        );
    }

    #[test]
    fn strict_rfc_8259_only() {
        for bad in [
            "",
            "   ",
            "{",
            "}",
            "[1,]",
            "[,1]",
            "{\"a\":1,}",
            "{\"a\" 1}",
            "{a:1}",
            "{'a':1}",
            "[1 2]",
            "tru",
            "nulll",
            "1 2",
            "{} {}",
            "{\"a\":1}x",
            "\u{feff}{}",
            "{} // c",
            "// c\n{}",
            "/* c */ {}",
            "{\"a\": /* c */ 1}",
            "[1, // c\n2]",
            "\u{a0}{}",
        ] {
            assert!(parse(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn comments_are_named_in_the_reason() {
        let error = parse("{\"a\": 1 // x\n}").unwrap_err();
        assert!(error.0.contains("comments"), "{error}");
    }

    #[test]
    fn duplicate_keys_are_refused_at_any_level() {
        assert!(parse("{\"a\":1,\"a\":2}").is_err());
        assert!(parse("{\"x\":{\"a\":1,\"a\":2}}").is_err());
        assert!(parse("[{\"a\":1,\"a\":2}]").is_err());
        assert!(parse("{\"a\":{\"b\":1},\"c\":{\"b\":2}}").is_ok());
    }

    #[test]
    fn nesting_is_bounded() {
        let nest = |n: usize| format!("{}{}", "[".repeat(n), "]".repeat(n));
        assert!(parse(&nest(MAX_DEPTH)).is_ok());
        assert!(parse(&nest(MAX_DEPTH + 1)).is_err());
        let deep = nest(100_000);
        assert!(parse(&deep).is_err());
    }

    #[test]
    fn size_is_bounded() {
        let filler = "a".repeat(MAX_BYTES);
        assert!(parse(&format!("\"{filler}\"")).is_err());
        let fits = "a".repeat(MAX_BYTES - 2);
        assert!(parse(&format!("\"{fits}\"")).is_ok());
        assert!(parse_limited("[1,2,3]", 6).is_err());
        assert!(parse_limited("[1,2,3]", 7).is_ok());
    }

    #[test]
    fn editing_helpers_keep_order() {
        let mut value = ok("{\"a\":1,\"b\":2}");
        value.set("a", Json::Num("9".into()));
        value.set("c", Json::Null);
        assert_eq!(compact(&value), "{\"a\":9,\"b\":2,\"c\":null}");
        assert!(value.remove("b"));
        assert!(!value.remove("b"));
        assert_eq!(compact(&value), "{\"a\":9,\"c\":null}");
        assert!(ok("{}").is_empty_object());
    }
}
