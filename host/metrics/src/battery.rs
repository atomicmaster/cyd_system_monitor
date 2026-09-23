// SPDX-License-Identifier: GPL-3.0-only

//! Battery readings and the pure pieces behind them (TB06): the charge/state
//! mapping from power-source values, and a defensive, bounded parser for
//! `system_profiler SPPowerDataType -json`. Nothing here runs a process or
//! touches the OS; the macOS adapter feeds these functions.
//!
//! The parser never trusts its input: it is size-bounded, its JSON reader has
//! depth and node limits, and every extracted field is type- and range-checked
//! so a wrong or missing field makes only that field unavailable.

/// Largest `system_profiler` output that is parsed; anything bigger is refused.
pub const MAX_PROFILER_BYTES: usize = 256 * 1024;
const MAX_JSON_DEPTH: usize = 24;
const MAX_JSON_NODES: usize = 20_000;

/// Charging state, numbered as on the wire (`protocol/CORE_PROTOCOL.md`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BatteryState {
    Unknown = 0,
    Charging = 1,
    Discharging = 2,
    Charged = 3,
    AcNotCharging = 4,
}

/// Battery condition, numbered as on the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BatteryCondition {
    Unknown = 0,
    Normal = 1,
    ServiceRecommended = 2,
    Poor = 3,
}

/// Current charge, from the public power-source API.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BatteryChargeReading {
    pub per_mille: u16,
    pub state: BatteryState,
    /// Minutes to empty (discharging) or to full (charging), when reported.
    pub time_remaining_min: Option<u16>,
}

/// Best-effort battery health. Each field is independently optional: the source
/// reports what it reports.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BatteryConditionReading {
    pub max_capacity_per_mille: Option<u16>,
    pub condition: BatteryCondition,
    pub cycle_count: Option<u16>,
}

/// What one `system_profiler SPPowerDataType` run says about the battery.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PowerProfile {
    /// The power section lists no battery: a batteryless host.
    NoBattery,
    Battery(BatteryConditionReading),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProfileError {
    /// Output above `MAX_PROFILER_BYTES`.
    TooLarge,
    /// Not JSON (or too deeply nested or too large a document).
    Malformed,
    /// Valid JSON with no usable `SPPowerDataType` array.
    NoPowerSection,
    /// A battery entry with none of maximum capacity, condition, or cycles.
    NoUsableFields,
}

/// Charge and state from the raw values of an internal-battery power source.
/// `None` when the capacities are impossible (a broken read, not a zero).
/// Charging wins; otherwise AC power means charged or idle-on-AC and battery
/// power means discharging. `time_to_empty`/`time_to_full` are minutes, with
/// negative meaning "still calculating".
pub fn charge_reading(
    current: i64,
    max: i64,
    is_charging: Option<bool>,
    is_charged: Option<bool>,
    on_ac: Option<bool>,
    time_to_empty: Option<i64>,
    time_to_full: Option<i64>,
) -> Option<BatteryChargeReading> {
    if max <= 0 || current < 0 || current > max {
        return None;
    }
    let per_mille = ((current * 1000 + max / 2) / max) as u16;
    let state = if is_charging == Some(true) {
        BatteryState::Charging
    } else if on_ac == Some(true) {
        if is_charged == Some(true) || per_mille >= 1000 {
            BatteryState::Charged
        } else {
            BatteryState::AcNotCharging
        }
    } else if on_ac == Some(false) {
        BatteryState::Discharging
    } else {
        BatteryState::Unknown
    };
    let minutes = match state {
        BatteryState::Charging => time_to_full,
        BatteryState::Discharging => time_to_empty,
        _ => None,
    };
    let time_remaining_min = minutes
        .filter(|m| (0..i64::from(u16::MAX)).contains(m))
        .map(|m| m as u16);
    Some(BatteryChargeReading {
        per_mille,
        state,
        time_remaining_min,
    })
}

/// Parses `system_profiler SPPowerDataType -json` output.
pub fn parse_power_profile(input: &[u8]) -> Result<PowerProfile, ProfileError> {
    if input.len() > MAX_PROFILER_BYTES {
        return Err(ProfileError::TooLarge);
    }
    let root = json::parse(input).ok_or(ProfileError::Malformed)?;
    let items = root
        .get("SPPowerDataType")
        .and_then(json::Value::as_array)
        .ok_or(ProfileError::NoPowerSection)?;
    let battery = items.iter().find(|item| {
        item.get("_name").and_then(json::Value::as_str) == Some("spbattery_information")
            || item.get("sppower_battery_health_info").is_some()
            || item.get("sppower_battery_charge_info").is_some()
    });
    let Some(battery) = battery else {
        return Ok(PowerProfile::NoBattery);
    };
    let health = battery.get("sppower_battery_health_info");
    let field = |key: &str| health.and_then(|h| h.get(key));
    let reading = BatteryConditionReading {
        max_capacity_per_mille: field("sppower_battery_health_maximum_capacity")
            .and_then(json::Value::as_str)
            .and_then(percent_to_per_mille),
        condition: field("sppower_battery_health")
            .and_then(json::Value::as_str)
            .map_or(BatteryCondition::Unknown, condition_from_text),
        cycle_count: field("sppower_battery_cycle_count")
            .and_then(json::Value::as_f64)
            .filter(|n| n.fract() == 0.0 && (0.0..f64::from(u16::MAX)).contains(n))
            .map(|n| n as u16),
    };
    if reading.max_capacity_per_mille.is_none()
        && reading.condition == BatteryCondition::Unknown
        && reading.cycle_count.is_none()
    {
        return Err(ProfileError::NoUsableFields);
    }
    Ok(PowerProfile::Battery(reading))
}

/// "91%" or "91.5 %" to per-mille; anything else, or above 100%, is `None`.
fn percent_to_per_mille(text: &str) -> Option<u16> {
    let number = text.trim().strip_suffix('%')?.trim();
    if number.is_empty() || !number.bytes().all(|b| b.is_ascii_digit() || b == b'.') {
        return None;
    }
    let percent: f64 = number.parse().ok()?;
    (0.0..=100.0)
        .contains(&percent)
        .then(|| (percent * 10.0).round() as u16)
}

/// The condition words macOS uses, old and new. Unrecognised text is unknown.
fn condition_from_text(text: &str) -> BatteryCondition {
    match text.trim().to_ascii_lowercase().as_str() {
        "normal" | "good" => BatteryCondition::Normal,
        "service recommended" | "replace soon" | "fair" => BatteryCondition::ServiceRecommended,
        "replace now" | "poor" | "check battery" | "service battery" => BatteryCondition::Poor,
        _ => BatteryCondition::Unknown,
    }
}

/// A small, bounded JSON reader: enough to walk `system_profiler` output
/// without a dependency. Returns `None` for anything malformed, too deep, too
/// large, or followed by trailing data.
mod json {
    use super::{MAX_JSON_DEPTH, MAX_JSON_NODES};

    #[derive(Debug, Clone, PartialEq)]
    pub enum Value {
        Null,
        Bool(bool),
        Number(f64),
        Str(String),
        Array(Vec<Value>),
        Object(Vec<(String, Value)>),
    }

    impl Value {
        pub fn get(&self, key: &str) -> Option<&Value> {
            match self {
                Value::Object(fields) => fields.iter().find(|(k, _)| k == key).map(|(_, v)| v),
                _ => None,
            }
        }
        pub fn as_str(&self) -> Option<&str> {
            match self {
                Value::Str(s) => Some(s),
                _ => None,
            }
        }
        pub fn as_f64(&self) -> Option<f64> {
            match self {
                Value::Number(n) => Some(*n),
                _ => None,
            }
        }
        pub fn as_array(&self) -> Option<&[Value]> {
            match self {
                Value::Array(a) => Some(a),
                _ => None,
            }
        }
    }

    pub fn parse(input: &[u8]) -> Option<Value> {
        let mut reader = Reader {
            input,
            pos: 0,
            nodes: 0,
        };
        let value = reader.value(0)?;
        reader.skip_space();
        (reader.pos == input.len()).then_some(value)
    }

    struct Reader<'a> {
        input: &'a [u8],
        pos: usize,
        nodes: usize,
    }

    impl Reader<'_> {
        fn skip_space(&mut self) {
            while matches!(self.input.get(self.pos), Some(b' ' | b'\t' | b'\n' | b'\r')) {
                self.pos += 1;
            }
        }
        fn eat(&mut self, byte: u8) -> bool {
            self.skip_space();
            if self.input.get(self.pos) == Some(&byte) {
                self.pos += 1;
                true
            } else {
                false
            }
        }
        fn literal(&mut self, word: &str, value: Value) -> Option<Value> {
            let end = self.pos.checked_add(word.len())?;
            (self.input.get(self.pos..end)? == word.as_bytes()).then(|| {
                self.pos = end;
                value
            })
        }
        fn value(&mut self, depth: usize) -> Option<Value> {
            self.nodes += 1;
            if depth > MAX_JSON_DEPTH || self.nodes > MAX_JSON_NODES {
                return None;
            }
            self.skip_space();
            match *self.input.get(self.pos)? {
                b'{' => {
                    self.pos += 1;
                    let mut fields = Vec::new();
                    if self.eat(b'}') {
                        return Some(Value::Object(fields));
                    }
                    loop {
                        self.skip_space();
                        let key = self.string()?;
                        if !self.eat(b':') {
                            return None;
                        }
                        fields.push((key, self.value(depth + 1)?));
                        if self.eat(b',') {
                            continue;
                        }
                        return self.eat(b'}').then_some(Value::Object(fields));
                    }
                }
                b'[' => {
                    self.pos += 1;
                    let mut items = Vec::new();
                    if self.eat(b']') {
                        return Some(Value::Array(items));
                    }
                    loop {
                        items.push(self.value(depth + 1)?);
                        if self.eat(b',') {
                            continue;
                        }
                        return self.eat(b']').then_some(Value::Array(items));
                    }
                }
                b'"' => self.string().map(Value::Str),
                b't' => self.literal("true", Value::Bool(true)),
                b'f' => self.literal("false", Value::Bool(false)),
                b'n' => self.literal("null", Value::Null),
                b'-' | b'0'..=b'9' => self.number(),
                _ => None,
            }
        }
        fn number(&mut self) -> Option<Value> {
            let start = self.pos;
            while matches!(
                self.input.get(self.pos),
                Some(b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9')
            ) {
                self.pos += 1;
            }
            let text = std::str::from_utf8(&self.input[start..self.pos]).ok()?;
            let number: f64 = text.parse().ok()?;
            number.is_finite().then_some(Value::Number(number))
        }
        fn hex4(&mut self) -> Option<u32> {
            let end = self.pos.checked_add(4)?;
            let digits = std::str::from_utf8(self.input.get(self.pos..end)?).ok()?;
            let value = u32::from_str_radix(digits, 16).ok()?;
            self.pos = end;
            Some(value)
        }
        fn string(&mut self) -> Option<String> {
            if self.input.get(self.pos) != Some(&b'"') {
                return None;
            }
            self.pos += 1;
            let mut bytes = Vec::new();
            loop {
                let byte = *self.input.get(self.pos)?;
                self.pos += 1;
                match byte {
                    b'"' => return String::from_utf8(bytes).ok(),
                    0..=0x1f => return None,
                    b'\\' => {
                        let escape = *self.input.get(self.pos)?;
                        self.pos += 1;
                        let ch = match escape {
                            b'"' => '"',
                            b'\\' => '\\',
                            b'/' => '/',
                            b'b' => '\u{8}',
                            b'f' => '\u{c}',
                            b'n' => '\n',
                            b'r' => '\r',
                            b't' => '\t',
                            b'u' => {
                                let unit = self.hex4()?;
                                if (0xD800..0xDC00).contains(&unit)
                                    && self.input.get(self.pos..self.pos + 2) == Some(b"\\u")
                                {
                                    self.pos += 2;
                                    let low = self.hex4()?;
                                    char::from_u32(
                                        0x10000
                                            + ((unit - 0xD800) << 10)
                                            + low.wrapping_sub(0xDC00),
                                    )
                                    .unwrap_or('\u{fffd}')
                                } else {
                                    char::from_u32(unit).unwrap_or('\u{fffd}')
                                }
                            }
                            _ => return None,
                        };
                        let mut buf = [0u8; 4];
                        bytes.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                    }
                    other => bytes.push(other),
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(name: &str) -> Vec<u8> {
        std::fs::read(format!(
            "{}/fixtures/system_profiler/{name}",
            env!("CARGO_MANIFEST_DIR")
        ))
        .unwrap_or_else(|e| panic!("{name}: {e}"))
    }

    fn battery(name: &str) -> BatteryConditionReading {
        match parse_power_profile(&fixture(name)) {
            Ok(PowerProfile::Battery(b)) => b,
            other => panic!("{name}: {other:?}"),
        }
    }

    #[test]
    fn a_healthy_battery_yields_capacity_condition_and_cycles() {
        assert_eq!(
            battery("battery-good.json"),
            BatteryConditionReading {
                max_capacity_per_mille: Some(910),
                condition: BatteryCondition::Normal,
                cycle_count: Some(312),
            }
        );
        let apple_silicon = battery("battery-normal-apple-silicon.json");
        assert_eq!(apple_silicon.max_capacity_per_mille, Some(1000));
        assert_eq!(apple_silicon.condition, BatteryCondition::Normal);
    }

    #[test]
    fn condition_words_map_to_the_wire_conditions() {
        for (file, condition, capacity) in [
            (
                "battery-service-recommended.json",
                BatteryCondition::ServiceRecommended,
                790,
            ),
            (
                "battery-replace-soon.json",
                BatteryCondition::ServiceRecommended,
                720,
            ),
            ("battery-replace-now.json", BatteryCondition::Poor, 580),
        ] {
            let b = battery(file);
            assert_eq!(
                (b.condition, b.max_capacity_per_mille),
                (condition, Some(capacity)),
                "{file}"
            );
        }
        assert_eq!(condition_from_text("  NORMAL "), BatteryCondition::Normal);
        assert_eq!(
            condition_from_text("Fair"),
            BatteryCondition::ServiceRecommended
        );
        assert_eq!(condition_from_text("Check Battery"), BatteryCondition::Poor);
        assert_eq!(condition_from_text(""), BatteryCondition::Unknown);
    }

    #[test]
    fn unknown_condition_text_and_partial_sections_make_only_those_fields_unavailable() {
        let b = battery("battery-unknown-condition-text.json");
        assert_eq!(b.condition, BatteryCondition::Unknown);
        assert_eq!(b.max_capacity_per_mille, Some(880));
        let b = battery("battery-capacity-only.json");
        assert_eq!(
            (b.max_capacity_per_mille, b.condition, b.cycle_count),
            (Some(900), BatteryCondition::Unknown, None)
        );
        let b = battery("battery-cycles-only.json");
        assert_eq!(
            (b.max_capacity_per_mille, b.condition, b.cycle_count),
            (None, BatteryCondition::Unknown, Some(77))
        );
    }

    #[test]
    fn wrongly_typed_or_out_of_range_fields_are_unavailable_not_panics() {
        // Every field has the wrong type: nothing usable, and no panic.
        assert_eq!(
            parse_power_profile(&fixture("battery-wrong-types.json")),
            Err(ProfileError::NoUsableFields)
        );
        // 150% capacity and a negative cycle count are refused; the condition stays.
        let b = battery("battery-out-of-range.json");
        assert_eq!(
            (b.max_capacity_per_mille, b.condition, b.cycle_count),
            (None, BatteryCondition::Normal, None)
        );
        let b = battery("battery-cycle-count-huge.json");
        assert_eq!(b.cycle_count, None);
        assert_eq!(b.max_capacity_per_mille, Some(950));
        assert_eq!(
            parse_power_profile(&fixture("battery-empty-health.json")),
            Err(ProfileError::NoUsableFields)
        );
    }

    #[test]
    fn a_batteryless_desktop_is_no_battery_not_an_error() {
        assert_eq!(
            parse_power_profile(&fixture("batteryless-desktop.json")),
            Ok(PowerProfile::NoBattery),
            "the AC charger's sppower_battery_* keys do not make a battery"
        );
    }

    #[test]
    fn missing_or_malformed_documents_are_errors_not_panics() {
        for (file, expected) in [
            ("missing-section.json", ProfileError::NoPowerSection),
            (
                "power-section-wrong-type.json",
                ProfileError::NoPowerSection,
            ),
            ("malformed-truncated.json", ProfileError::Malformed),
            ("malformed-not-json.json", ProfileError::Malformed),
            ("malformed-trailing-garbage.json", ProfileError::Malformed),
            ("empty.json", ProfileError::Malformed),
        ] {
            assert_eq!(parse_power_profile(&fixture(file)), Err(expected), "{file}");
        }
    }

    #[test]
    fn escaped_strings_decode() {
        let b = battery("battery-escaped-strings.json");
        assert_eq!(b.max_capacity_per_mille, Some(910), "\\u0031 is a 1");
        assert_eq!(b.condition, BatteryCondition::Normal);
    }

    #[test]
    fn oversized_output_is_refused_before_parsing() {
        let mut big = fixture("battery-good.json");
        big.resize(MAX_PROFILER_BYTES + 1, b' ');
        assert_eq!(parse_power_profile(&big), Err(ProfileError::TooLarge));
        // Exactly at the limit is still parsed.
        big.truncate(MAX_PROFILER_BYTES);
        assert!(parse_power_profile(&big).is_ok());
    }

    #[test]
    fn hostile_nesting_and_size_are_bounded() {
        let deep = format!("{}{}", "[".repeat(10_000), "]".repeat(10_000));
        assert_eq!(
            parse_power_profile(deep.as_bytes()),
            Err(ProfileError::Malformed)
        );
        let wide = format!("[{}0]", "0,".repeat(MAX_JSON_NODES));
        assert_eq!(
            parse_power_profile(wide.as_bytes()),
            Err(ProfileError::Malformed)
        );
        for hostile in [
            &b"{\"a\":\"\\ud800\"}"[..],
            b"{\"a\":\"\\u12\"}",
            b"{\"a\":\"\xff\"}",
            b"{\"a\":1e999}",
            b"{\"SPPowerDataType\":[",
            b"\x00\x01\x02",
        ] {
            let _ = parse_power_profile(hostile); // must not panic
        }
    }

    #[test]
    fn percent_text_is_parsed_strictly() {
        assert_eq!(percent_to_per_mille("91%"), Some(910));
        assert_eq!(percent_to_per_mille(" 100 % "), Some(1000));
        assert_eq!(percent_to_per_mille("91.5%"), Some(915));
        assert_eq!(percent_to_per_mille("0%"), Some(0));
        for bad in ["", "%", "91", "101%", "-5%", "abc%", "9e1%", "1.2.3%"] {
            assert_eq!(percent_to_per_mille(bad), None, "{bad}");
        }
    }

    #[test]
    fn charge_state_follows_charging_then_the_power_source() {
        let read = |c, m, charging, charged, ac, empty, full| {
            charge_reading(c, m, charging, charged, ac, empty, full).unwrap()
        };
        let r = read(
            87,
            100,
            Some(true),
            Some(false),
            Some(true),
            Some(-1),
            Some(95),
        );
        assert_eq!(
            (r.per_mille, r.state, r.time_remaining_min),
            (870, BatteryState::Charging, Some(95))
        );
        let r = read(
            64,
            100,
            Some(false),
            Some(false),
            Some(false),
            Some(140),
            Some(0),
        );
        assert_eq!(
            (r.state, r.time_remaining_min),
            (BatteryState::Discharging, Some(140))
        );
        let r = read(
            100,
            100,
            Some(false),
            Some(true),
            Some(true),
            Some(0),
            Some(0),
        );
        assert_eq!(
            (r.state, r.time_remaining_min),
            (BatteryState::Charged, None)
        );
        let r = read(80, 100, Some(false), Some(false), Some(true), None, None);
        assert_eq!(r.state, BatteryState::AcNotCharging);
        let r = read(50, 100, None, None, None, None, None);
        assert_eq!(r.state, BatteryState::Unknown);
        // Still calculating (negative) and out-of-range minutes are unknown.
        let r = read(64, 100, Some(false), None, Some(false), Some(-1), None);
        assert_eq!(r.time_remaining_min, None);
        let r = read(64, 100, Some(false), None, Some(false), Some(70_000), None);
        assert_eq!(r.time_remaining_min, None);
    }

    #[test]
    fn impossible_capacities_are_a_broken_read_not_a_zero() {
        for (current, max) in [(50, 0), (50, -1), (-1, 100), (101, 100)] {
            assert_eq!(
                charge_reading(current, max, None, None, None, None, None),
                None
            );
        }
        // A genuinely empty battery is a measured zero.
        assert_eq!(
            charge_reading(0, 100, Some(false), None, Some(false), None, None)
                .unwrap()
                .per_mille,
            0
        );
        // Capacities other than a percentage scale to per-mille.
        assert_eq!(
            charge_reading(3000, 6000, None, None, None, None, None)
                .unwrap()
                .per_mille,
            500
        );
    }
}
