// SPDX-License-Identifier: GPL-3.0-only

//! The hook adapters and the pure logic over their config text: the adapter
//! table (one entry per agent), classifying a config as installed, partial or
//! outdated, and the install and remove edits. Nothing here touches the file
//! system; `files.rs` reads and writes, `mod.rs` ties the two together.
//!
//! A hook is "ours" when its command contains `agent-event <slug> ` (the
//! agent's slug followed by a space). That is how status, re-pointing and
//! removal find our entries among an agent's other hooks, which are never
//! touched. Adding an agent is one entry in [`ADAPTERS`] plus a config
//! fixture in `fixtures/hooks/`.

use metrics::agents::{Agent, HookActivity};

use super::json::{self, Json};

/// How one event's entry is shaped in the config.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shape {
    /// `{"matcher"?: m, "hooks": [handler]}`.
    Group,
    /// The handler itself, directly in the event's list (Gemini's flat events).
    Flat,
}

#[derive(Debug, Clone, Copy)]
pub struct EventSpec {
    pub event: &'static str,
    pub activity: HookActivity,
    pub matcher: Option<&'static str>,
    pub shape: Shape,
}

/// What of the file is ours.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Scope {
    /// Only our handlers, among the file's other settings and hooks.
    Shared,
    /// The whole file: created, replaced and deleted as one.
    WholeFile,
    /// One top-level key: inserted, replaced and deleted as one.
    OwnKey,
}

#[derive(Debug, Clone, Copy)]
pub enum Format {
    /// JSON hooks: the events live in the object at `root[key]`.
    Json {
        key: &'static str,
        scope: Scope,
        /// Seconds, for agents whose format has a handler timeout.
        timeout: Option<u32>,
    },
    /// A JavaScript plugin generated from `host/integrations/opencode`.
    Plugin,
}

#[derive(Debug, Clone, Copy)]
pub struct Adapter {
    pub agent: Agent,
    /// Home-relative; the agent counts as installed when this directory exists.
    pub config_dir: &'static str,
    /// Home-relative config file.
    pub file: &'static str,
    pub format: Format,
    pub events: &'static [EventSpec],
}

impl Adapter {
    pub fn slug(&self) -> &'static str {
        self.agent.slug()
    }
}

const fn group(
    event: &'static str,
    activity: HookActivity,
    matcher: Option<&'static str>,
) -> EventSpec {
    EventSpec {
        event,
        activity,
        matcher,
        shape: Shape::Group,
    }
}

const fn flat(event: &'static str, activity: HookActivity) -> EventSpec {
    EventSpec {
        event,
        activity,
        matcher: None,
        shape: Shape::Flat,
    }
}

use HookActivity::{Busy, Idle};

/// The event to busy/idle maps are those of `docs/agent-integrations.md`.
pub const ADAPTERS: &[Adapter] = &[
    Adapter {
        agent: Agent::Codex,
        config_dir: ".codex",
        file: ".codex/hooks.json",
        format: Format::Json {
            key: "hooks",
            scope: Scope::Shared,
            timeout: Some(5),
        },
        events: &[
            group("UserPromptSubmit", Busy, None),
            group("PreToolUse", Busy, None),
            group("PostToolUse", Busy, None),
            group("PermissionRequest", Idle, None),
            group("Stop", Idle, None),
            group("SessionEnd", Idle, None),
            group("Interrupt", Idle, None),
        ],
    },
    Adapter {
        agent: Agent::Claude,
        config_dir: ".claude",
        file: ".claude/settings.json",
        format: Format::Json {
            key: "hooks",
            scope: Scope::Shared,
            timeout: Some(5),
        },
        events: &[
            group("UserPromptSubmit", Busy, None),
            group("PreToolUse", Busy, Some("*")),
            group("PostToolUse", Busy, Some("*")),
            group("Stop", Idle, None),
            group("Notification", Idle, None),
            group("SessionEnd", Idle, None),
        ],
    },
    Adapter {
        agent: Agent::Grok,
        config_dir: ".grok",
        file: ".grok/hooks/cyd-system-monitor.json",
        format: Format::Json {
            key: "hooks",
            scope: Scope::WholeFile,
            timeout: None,
        },
        events: &[
            group("UserPromptSubmit", Busy, None),
            group("PreToolUse", Busy, None),
            group("PostToolUse", Busy, None),
            group("PostToolUseFailure", Busy, None),
            group("Notification", Idle, Some("permission_prompt|idle_prompt")),
            group("Stop", Idle, None),
            group("StopFailure", Idle, None),
            group("StopCancelled", Idle, None),
            group("SessionEnd", Idle, None),
        ],
    },
    Adapter {
        agent: Agent::Gemini,
        config_dir: ".gemini",
        file: ".gemini/config/hooks.json",
        format: Format::Json {
            key: "cyd-activity",
            scope: Scope::OwnKey,
            timeout: None,
        },
        events: &[
            flat("PreInvocation", Busy),
            group("PreToolUse", Busy, Some(".*")),
            group("PostToolUse", Busy, Some(".*")),
            flat("Stop", Idle),
        ],
    },
    Adapter {
        agent: Agent::OpenCode,
        config_dir: ".config/opencode",
        file: ".config/opencode/plugins/cyd-system-monitor.js",
        format: Format::Plugin,
        events: &[],
    },
];

pub fn adapter_for(slug: &str) -> Option<&'static Adapter> {
    ADAPTERS.iter().find(|a| a.slug() == slug)
}

fn activity_word(activity: HookActivity) -> &'static str {
    match activity {
        HookActivity::Busy => "busy",
        HookActivity::Idle => "idle",
    }
}

/// `'<binary>' agent-event <slug> <busy|idle> 2>/dev/null || true`: the
/// failure of a missing binary never reaches the agent.
pub fn command_line(binary: &str, slug: &str, activity: HookActivity) -> String {
    format!(
        "'{}' agent-event {slug} {} 2>/dev/null || true",
        binary.replace('\'', "'\\''"),
        activity_word(activity)
    )
}

fn marker(slug: &str) -> String {
    format!("agent-event {slug} ")
}

pub fn is_ours(command: &str, slug: &str) -> bool {
    command.contains(&marker(slug))
}

/// The binary path and reported activity in one of our commands.
fn parse_command(command: &str, slug: &str) -> Option<(String, Option<HookActivity>)> {
    let marker = marker(slug);
    let at = command.find(&marker)?;
    let head = command[..at].trim();
    let path = match head.strip_prefix('\'').and_then(|h| h.strip_suffix('\'')) {
        Some(quoted) => quoted.replace("'\\''", "'"),
        None => head.to_string(),
    };
    let activity = match command[at + marker.len()..].split_whitespace().next() {
        Some("busy") => Some(HookActivity::Busy),
        Some("idle") => Some(HookActivity::Idle),
        _ => None,
    };
    Some((path, activity))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EventState {
    Ok,
    Missing,
    Outdated(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HookState {
    Installed,
    /// Some events have no hook of ours; the text lists them.
    Partial(String),
    /// Ours, but pointing elsewhere, at a missing binary, or reporting the
    /// wrong state.
    Outdated(String),
    NotInstalled,
    AgentNotInstalled,
    Unreadable(String),
}

impl HookState {
    /// Installed, or nothing to install on this Mac.
    pub fn is_fine(&self) -> bool {
        matches!(self, HookState::Installed | HookState::AgentNotInstalled)
    }
}

/// A handler is an object with a string `command`; an event's list holds
/// handlers directly (flat) or groups with a `hooks` list of handlers.
fn handler_command(item: &Json) -> Option<&str> {
    item.get("command")?.as_str()
}

fn for_each_handler(list: &[Json], f: &mut dyn FnMut(&Json)) {
    for item in list {
        if handler_command(item).is_some() {
            f(item);
        } else if let Some(Json::Arr(inner)) = item.get("hooks") {
            inner
                .iter()
                .filter(|h| handler_command(h).is_some())
                .for_each(&mut *f);
        }
    }
}

fn for_each_handler_mut(list: &mut [Json], f: &mut dyn FnMut(&mut Json)) {
    for item in list {
        if handler_command(item).is_some() {
            f(item);
        } else if let Some(Json::Arr(inner)) = item.get_mut("hooks") {
            for handler in inner.iter_mut().filter(|h| handler_command(h).is_some()) {
                f(handler);
            }
        }
    }
}

/// The object holding the events: `root[key]`, empty when absent.
fn container<'a>(root: &'a Json, key: &str) -> Result<Option<&'a Json>, String> {
    if !matches!(root, Json::Obj(_)) {
        return Err("the top level is not an object".to_string());
    }
    match root.get(key) {
        None => Ok(None),
        Some(found @ Json::Obj(_)) => Ok(Some(found)),
        Some(_) => Err(format!("\"{key}\" is not an object")),
    }
}

fn event_list<'a>(container: Option<&'a Json>, event: &str) -> Result<&'a [Json], String> {
    match container.and_then(|c| c.get(event)) {
        None => Ok(&[]),
        Some(Json::Arr(items)) => Ok(items),
        Some(_) => Err(format!("\"{event}\" is not a list")),
    }
}

fn judge(
    command: &str,
    slug: &str,
    want: HookActivity,
    binary: &str,
    exists: &dyn Fn(&str) -> bool,
) -> EventState {
    let Some((path, activity)) = parse_command(command, slug) else {
        return EventState::Outdated("unrecognised command".to_string());
    };
    if path != binary {
        return EventState::Outdated(format!("pointing at {path}"));
    }
    if activity != Some(want) {
        return EventState::Outdated(format!(
            "reporting the wrong state (expected {})",
            activity_word(want)
        ));
    }
    if !exists(&path) {
        return EventState::Outdated(format!("binary {path} does not exist"));
    }
    EventState::Ok
}

/// Each of the adapter's events against a parsed config.
pub fn inspect_json(
    adapter: &Adapter,
    root: &Json,
    binary: &str,
    exists: &dyn Fn(&str) -> bool,
) -> Result<Vec<(&'static str, EventState)>, String> {
    let Format::Json { key, .. } = adapter.format else {
        return Err("not a JSON adapter".to_string());
    };
    let container = container(root, key)?;
    let mut states = Vec::new();
    for spec in adapter.events {
        let list = event_list(container, spec.event)?;
        let mut seen_ok = false;
        let mut bad = None;
        for_each_handler(list, &mut |handler| {
            let command = handler_command(handler).unwrap_or_default();
            if !is_ours(command, adapter.slug()) {
                return;
            }
            match judge(command, adapter.slug(), spec.activity, binary, exists) {
                EventState::Outdated(why) => bad = bad.take().or(Some(why)),
                _ => seen_ok = true,
            }
        });
        // One bad handler of ours makes the event outdated.
        let worst = match (bad, seen_ok) {
            (Some(why), _) => EventState::Outdated(why),
            (None, true) => EventState::Ok,
            (None, false) => EventState::Missing,
        };
        states.push((spec.event, worst));
    }
    Ok(states)
}

fn summarize(states: &[(&'static str, EventState)]) -> HookState {
    let missing: Vec<&str> = states
        .iter()
        .filter(|(_, s)| *s == EventState::Missing)
        .map(|(e, _)| *e)
        .collect();
    // Events with the same complaint are listed together.
    let mut grouped: Vec<(&str, Vec<&str>)> = Vec::new();
    for (event, state) in states {
        if let EventState::Outdated(why) = state {
            match grouped.iter_mut().find(|(w, _)| w == why) {
                Some((_, events)) => events.push(event),
                None => grouped.push((why, vec![event])),
            }
        }
    }
    let outdated: Vec<String> = grouped
        .iter()
        .map(|(why, events)| format!("{}: {why}", events.join(", ")))
        .collect();
    if missing.len() == states.len() {
        HookState::NotInstalled
    } else if !missing.is_empty() {
        let mut text = format!("missing {}", missing.join(", "));
        if !outdated.is_empty() {
            text.push_str(&format!("; outdated: {}", outdated.join(", ")));
        }
        HookState::Partial(text)
    } else if !outdated.is_empty() {
        HookState::Outdated(outdated.join("; "))
    } else {
        HookState::Installed
    }
}

const PLUGIN_TEMPLATE: &str = include_str!("../../../integrations/opencode/cyd-system-monitor.js");
const PLUGIN_PLACEHOLDER: &str = "/absolute/path/to/cyd-host";
const PLUGIN_ID: &str = "id: \"cyd-system-monitor\"";
const PLUGIN_CALL: &str = "\"agent-event\", \"opencode\"";

fn js_escape(text: &str) -> String {
    let mut out = String::new();
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 || matches!(c, '\u{2028}' | '\u{2029}' | '\u{7f}') => {
                out.push_str(&format!("\\u{:04x}", c as u32))
            }
            c => out.push(c),
        }
    }
    out
}

/// The OpenCode plugin with the binary path filled in.
pub fn plugin_source(binary: &str) -> String {
    PLUGIN_TEMPLATE.replace(PLUGIN_PLACEHOLDER, &js_escape(binary))
}

fn plugin_is_ours(text: &str) -> bool {
    text.contains(PLUGIN_ID) && text.contains(PLUGIN_CALL)
}

/// The path in the plugin's `|| "<path>"` default, for the "points at" note.
fn plugin_path(text: &str) -> Option<String> {
    let rest = &text[text.find("|| \"")? + 4..];
    let mut out = String::new();
    let mut chars = rest.chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(out),
            '\\' => match chars.next()? {
                'u' => {
                    let hex: String = chars.by_ref().take(4).collect();
                    out.push(char::from_u32(u32::from_str_radix(&hex, 16).ok()?)?);
                }
                other => out.push(other),
            },
            c => out.push(c),
        }
    }
    None
}

/// Classifies a config file's text (`None`: no file) for an adapter.
pub fn classify(
    adapter: &Adapter,
    text: Option<&str>,
    binary: &str,
    exists: &dyn Fn(&str) -> bool,
) -> HookState {
    let Some(text) = text else {
        return HookState::NotInstalled;
    };
    if let Format::Plugin = adapter.format {
        if text == plugin_source(binary) {
            return if exists(binary) {
                HookState::Installed
            } else {
                HookState::Outdated(format!("binary {binary} does not exist"))
            };
        }
        if plugin_is_ours(text) {
            let at = plugin_path(text).unwrap_or_else(|| "an unknown path".to_string());
            return HookState::Outdated(format!(
                "plugin differs from this cyd-host's, pointing at {at}"
            ));
        }
        return HookState::Unreadable("the file is not the cyd-host plugin".to_string());
    }
    let root = match json::parse(text) {
        Ok(root) => root,
        Err(error) => return HookState::Unreadable(error.to_string()),
    };
    match inspect_json(adapter, &root, binary, exists) {
        Ok(states) => summarize(&states),
        Err(reason) => HookState::Unreadable(reason),
    }
}

/// What to do to a config file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Edit {
    Unchanged,
    Write(String),
    Delete,
}

fn new_entry(adapter: &Adapter, spec: &EventSpec, binary: &str, timeout: Option<u32>) -> Json {
    let mut handler = Json::Obj(vec![
        ("type".into(), Json::str("command")),
        (
            "command".into(),
            Json::Str(command_line(binary, adapter.slug(), spec.activity)),
        ),
    ]);
    if let Some(seconds) = timeout {
        handler.set("timeout", Json::Num(seconds.to_string()));
    }
    match spec.shape {
        Shape::Flat => handler,
        Shape::Group => {
            let mut group = Json::Obj(vec![]);
            if let Some(matcher) = spec.matcher {
                group.set("matcher", Json::str(matcher));
            }
            group.set("hooks", Json::Arr(vec![handler]));
            group
        }
    }
}

/// Points every one of our handlers at `binary` and adds a group for each
/// event that has none. Other hooks and settings are left as they are.
fn merge(adapter: &Adapter, root: &mut Json, binary: &str) -> Result<(), String> {
    let Format::Json { key, timeout, .. } = adapter.format else {
        return Err("not a JSON adapter".to_string());
    };
    container(root, key)?;
    if root.get(key).is_none() {
        root.set(key, Json::Obj(vec![]));
    }
    let Some(events) = root.get_mut(key) else {
        return Err("internal: container vanished".to_string());
    };
    for spec in adapter.events {
        if events.get(spec.event).is_none() {
            events.set(spec.event, Json::Arr(vec![]));
        }
        let Some(Json::Arr(list)) = events.get_mut(spec.event) else {
            return Err(format!("\"{}\" is not a list", spec.event));
        };
        let want = command_line(binary, adapter.slug(), spec.activity);
        let mut found = false;
        for_each_handler_mut(list, &mut |handler| {
            let ours = handler_command(handler).is_some_and(|c| is_ours(c, adapter.slug()));
            if ours {
                found = true;
                handler.set("command", Json::Str(want.clone()));
                if let (Some(seconds), None) = (timeout, handler.get("timeout")) {
                    handler.set("timeout", Json::Num(seconds.to_string()));
                }
            }
        });
        if !found {
            list.push(new_entry(adapter, spec, binary, timeout));
        }
    }
    Ok(())
}

/// The edit that makes the config hold our hooks for `binary`. A file that
/// already does is `Unchanged` (and is not reformatted).
pub fn install(adapter: &Adapter, existing: Option<&str>, binary: &str) -> Result<Edit, String> {
    let Format::Json { scope, .. } = adapter.format else {
        let want = plugin_source(binary);
        return match existing {
            Some(text) if text == want => Ok(Edit::Unchanged),
            Some(text) if !plugin_is_ours(text) => {
                Err("a file at that path is not the cyd-host plugin; left alone".to_string())
            }
            _ => Ok(Edit::Write(want)),
        };
    };
    let parsed = match existing {
        Some(text) => Some(json::parse(text).map_err(|e| e.to_string())?),
        None => None,
    };
    // A whole-file adapter owns every byte, so it starts from nothing.
    let mut root = match (&parsed, scope) {
        (Some(root), Scope::Shared | Scope::OwnKey) => root.clone(),
        _ => Json::Obj(vec![]),
    };
    merge(adapter, &mut root, binary)?;
    if parsed.as_ref() == Some(&root) {
        Ok(Edit::Unchanged)
    } else {
        Ok(Edit::Write(json::pretty(&root)))
    }
}

/// Drops our handlers from one event's list; true when any went. Groups and
/// events left empty by that are dropped by the caller.
fn strip_ours(list: &mut Vec<Json>, slug: &str) -> bool {
    let mut removed = false;
    let ours = |item: &Json| handler_command(item).is_some_and(|c| is_ours(c, slug));
    list.retain_mut(|item| {
        if handler_command(item).is_some() {
            if ours(item) {
                removed = true;
                return false;
            }
        } else if let Some(Json::Arr(inner)) = item.get_mut("hooks") {
            let before = inner.len();
            inner.retain(|h| !ours(h));
            if inner.len() != before {
                removed = true;
                return !inner.is_empty();
            }
        }
        true
    });
    removed
}

/// The edit that removes our hooks and nothing else.
pub fn remove(adapter: &Adapter, existing: Option<&str>) -> Result<Edit, String> {
    let Some(text) = existing else {
        return Ok(Edit::Unchanged);
    };
    let Format::Json { key, scope, .. } = adapter.format else {
        return if plugin_is_ours(text) {
            Ok(Edit::Delete)
        } else {
            Err("a file at that path is not the cyd-host plugin; left alone".to_string())
        };
    };
    if scope == Scope::WholeFile {
        return Ok(Edit::Delete);
    }
    let mut root = json::parse(text).map_err(|e| e.to_string())?;
    container(&root, key)?;
    let mut removed = false;
    if scope == Scope::OwnKey {
        removed = root.remove(key);
    } else if let Some(Json::Obj(fields)) = root.get_mut(key) {
        fields.retain_mut(|(_, value)| {
            let Json::Arr(list) = value else { return true };
            let touched = strip_ours(list, adapter.slug());
            removed |= touched;
            !(touched && list.is_empty())
        });
        if removed && fields.is_empty() {
            root.remove(key);
        }
    }
    if !removed {
        Ok(Edit::Unchanged)
    } else if root.is_empty_object() {
        Ok(Edit::Delete)
    } else {
        Ok(Edit::Write(json::pretty(&root)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const BIN: &str = "/opt/cyd/cyd-host";

    fn present(_: &str) -> bool {
        true
    }

    fn adapter(slug: &str) -> &'static Adapter {
        adapter_for(slug).expect("adapter")
    }

    fn state(slug: &str, text: Option<&str>) -> HookState {
        classify(adapter(slug), text, BIN, &present)
    }

    fn installed(slug: &str, base: Option<&str>) -> String {
        match install(adapter(slug), base, BIN) {
            Ok(Edit::Write(text)) => text,
            other => panic!("{slug}: {other:?}"),
        }
    }

    #[test]
    fn command_line_quotes_the_path_and_discards_failure() {
        assert_eq!(
            command_line("/a b/it's/cyd-host", "claude", Busy),
            "'/a b/it'\\''s/cyd-host' agent-event claude busy 2>/dev/null || true"
        );
        let command = command_line("/a b/it's/cyd-host", "claude", Idle);
        assert_eq!(
            parse_command(&command, "claude"),
            Some(("/a b/it's/cyd-host".to_string(), Some(Idle)))
        );
    }

    #[test]
    fn ours_means_the_slug_then_a_space() {
        assert!(is_ours("'/x' agent-event claude busy", "claude"));
        assert!(is_ours(
            "/x agent-event codex idle 2>/dev/null || true",
            "codex"
        ));
        assert!(!is_ours("/x agent-event claude busy", "codex"));
        assert!(!is_ours("/x agent-event claudex busy", "claude"));
        assert!(!is_ours("/x agent-event claude", "claude"));
        assert!(!is_ours("herdr agent report --agent claude", "claude"));
    }

    #[test]
    fn the_table_is_complete_and_consistent() {
        assert_eq!(ADAPTERS.len(), metrics::agents::AGENT_COUNT);
        for agent in metrics::agents::AGENTS {
            assert!(ADAPTERS.iter().any(|a| a.agent == agent), "{agent:?}");
        }
        for a in ADAPTERS {
            assert!(a.file.starts_with(a.config_dir), "{}", a.slug());
            match a.format {
                Format::Json { .. } => {
                    assert!(a.events.iter().any(|e| e.activity == Busy), "{}", a.slug());
                    assert!(a.events.iter().any(|e| e.activity == Idle), "{}", a.slug());
                }
                Format::Plugin => assert!(a.events.is_empty()),
            }
        }
    }

    #[test]
    fn event_maps_match_the_documented_ones() {
        let map = |slug: &str, activity| -> Vec<&str> {
            adapter(slug)
                .events
                .iter()
                .filter(|e| e.activity == activity)
                .map(|e| e.event)
                .collect()
        };
        assert_eq!(
            map("claude", Busy),
            ["UserPromptSubmit", "PreToolUse", "PostToolUse"]
        );
        assert_eq!(map("claude", Idle), ["Stop", "Notification", "SessionEnd"]);
        assert_eq!(
            map("codex", Busy),
            ["UserPromptSubmit", "PreToolUse", "PostToolUse"]
        );
        assert_eq!(
            map("codex", Idle),
            ["PermissionRequest", "Stop", "SessionEnd", "Interrupt"]
        );
        assert_eq!(
            map("grok", Busy),
            [
                "UserPromptSubmit",
                "PreToolUse",
                "PostToolUse",
                "PostToolUseFailure"
            ]
        );
        assert_eq!(
            map("grok", Idle),
            [
                "Notification",
                "Stop",
                "StopFailure",
                "StopCancelled",
                "SessionEnd"
            ]
        );
        assert_eq!(
            map("gemini", Busy),
            ["PreInvocation", "PreToolUse", "PostToolUse"]
        );
        assert_eq!(map("gemini", Idle), ["Stop"]);
    }

    #[test]
    fn missing_file_is_not_installed() {
        for a in ADAPTERS {
            assert_eq!(
                state(a.slug(), None),
                HookState::NotInstalled,
                "{}",
                a.slug()
            );
        }
    }

    #[test]
    fn install_into_nothing_is_installed_for_every_adapter() {
        for a in ADAPTERS {
            let text = installed(a.slug(), None);
            assert_eq!(
                state(a.slug(), Some(&text)),
                HookState::Installed,
                "{}",
                a.slug()
            );
        }
    }

    #[test]
    fn a_missing_event_is_partial() {
        let text = installed("claude", None);
        let mut root = json::parse(&text).unwrap();
        root.get_mut("hooks").unwrap().remove("Stop");
        root.get_mut("hooks").unwrap().remove("SessionEnd");
        let partial = json::pretty(&root);
        assert_eq!(
            state("claude", Some(&partial)),
            HookState::Partial("missing Stop, SessionEnd".to_string())
        );
    }

    #[test]
    fn another_path_or_a_missing_binary_is_outdated() {
        let text = installed("codex", None);
        let moved = text.replace(BIN, "/old/place/cyd-host");
        assert!(
            matches!(state("codex", Some(&moved)), HookState::Outdated(why) if why.contains("pointing at /old/place/cyd-host"))
        );
        let gone = classify(adapter("codex"), Some(&text), BIN, &|_| false);
        assert!(matches!(gone, HookState::Outdated(why) if why.contains("does not exist")));
    }

    #[test]
    fn a_wrong_state_is_outdated() {
        let text =
            installed("claude", None).replace("agent-event claude idle", "agent-event claude busy");
        assert!(
            matches!(state("claude", Some(&text)), HookState::Outdated(why) if why.contains("expected idle"))
        );
    }

    #[test]
    fn foreign_hooks_only_is_not_installed() {
        let text = r#"{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"herdr agent report --agent claude idle"}]}]}}"#;
        assert_eq!(state("claude", Some(text)), HookState::NotInstalled);
        assert_eq!(state("claude", Some("{}")), HookState::NotInstalled);
    }

    #[test]
    fn another_agents_hooks_do_not_count() {
        let text = installed("claude", None).replace("agent-event claude", "agent-event codex");
        assert_eq!(state("claude", Some(&text)), HookState::NotInstalled);
    }

    #[test]
    fn malformed_or_unexpected_configs_are_unreadable() {
        for a in ADAPTERS
            .iter()
            .filter(|a| matches!(a.format, Format::Json { .. }))
        {
            for bad in [
                "",
                "{",
                "{\"hooks\": {} // comment\n}",
                "{\"hooks\": {},}",
                "{\"hooks\": {}, \"hooks\": {}}",
                "[]",
                "42",
            ] {
                assert!(
                    matches!(state(a.slug(), Some(bad)), HookState::Unreadable(_)),
                    "{} {bad:?}",
                    a.slug()
                );
            }
        }
        for (slug, bad) in [
            ("claude", r#"{"hooks": []}"#),
            ("claude", r#"{"hooks": {"Stop": {}}}"#),
            ("gemini", r#"{"cyd-activity": []}"#),
        ] {
            assert!(
                matches!(state(slug, Some(bad)), HookState::Unreadable(_)),
                "{bad}"
            );
            assert!(install(adapter(slug), Some(bad), BIN).is_err(), "{bad}");
        }
    }

    #[test]
    fn unreadable_configs_are_never_rewritten() {
        for bad in ["{\"hooks\": {} // c\n}", "{", "[]"] {
            assert!(install(adapter("claude"), Some(bad), BIN).is_err());
            assert!(remove(adapter("claude"), Some(bad)).is_err());
        }
    }

    const CLAUDE_FOREIGN: &str = r#"{
  "model": "opus",
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {
            "type": "command",
            "command": "herdr agent report"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "tldraw-notify stop"
          }
        ]
      }
    ]
  },
  "env": {
    "A": "1"
  }
}
"#;

    #[test]
    fn install_keeps_foreign_hooks_and_settings_and_remove_restores_the_file() {
        let after = installed("claude", Some(CLAUDE_FOREIGN));
        assert!(after.contains("herdr agent report") && after.contains("tldraw-notify stop"));
        assert!(after.contains("\"model\": \"opus\"") && after.contains("\"A\": \"1\""));
        assert_eq!(state("claude", Some(&after)), HookState::Installed);
        // Foreign hooks keep their place ahead of ours.
        assert!(
            after.find("herdr agent report").unwrap()
                < after.find("agent-event claude busy").unwrap()
        );
        assert_eq!(
            remove(adapter("claude"), Some(&after)),
            Ok(Edit::Write(CLAUDE_FOREIGN.to_string()))
        );
    }

    #[test]
    fn a_second_install_changes_nothing() {
        for a in ADAPTERS {
            let once = installed(a.slug(), None);
            assert_eq!(
                install(a, Some(&once), BIN),
                Ok(Edit::Unchanged),
                "{}",
                a.slug()
            );
        }
        let once = installed("claude", Some(CLAUDE_FOREIGN));
        assert_eq!(
            install(adapter("claude"), Some(&once), BIN),
            Ok(Edit::Unchanged)
        );
    }

    #[test]
    fn an_installed_file_in_another_format_is_not_reformatted() {
        let once = installed("codex", None);
        let compact = json::compact(&json::parse(&once).unwrap());
        assert_eq!(
            install(adapter("codex"), Some(&compact), BIN),
            Ok(Edit::Unchanged)
        );
    }

    #[test]
    fn install_repoints_ours_in_place_and_keeps_neighbours() {
        let text = r#"{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"keep me"},{"type":"command","command":"/old/cyd-host agent-event claude idle","timeout":9}]}]}}"#;
        let after = installed("claude", Some(text));
        assert!(!after.contains("/old/cyd-host"));
        assert!(after.contains("keep me"));
        assert!(
            after.contains("\"timeout\": 9"),
            "an existing timeout is kept"
        );
        assert_eq!(state("claude", Some(&after)), HookState::Installed);
        // Stop had a group already: it was re-pointed, not duplicated.
        assert_eq!(after.matches("agent-event claude idle").count(), 3);
    }

    #[test]
    fn remove_drops_only_ours_and_prunes_what_it_emptied() {
        let after = installed("claude", Some(CLAUDE_FOREIGN));
        let Ok(Edit::Write(back)) = remove(adapter("claude"), Some(&after)) else {
            panic!("expected a write")
        };
        assert!(!back.contains("agent-event"));
        assert!(!back.contains("Notification") && !back.contains("SessionEnd"));
        // Nothing of ours: nothing to do, and a second removal is a no-op.
        assert_eq!(remove(adapter("claude"), Some(&back)), Ok(Edit::Unchanged));
        assert_eq!(remove(adapter("claude"), None), Ok(Edit::Unchanged));
    }

    #[test]
    fn removing_the_last_hook_of_a_file_we_made_deletes_the_file() {
        for slug in ["claude", "codex", "gemini", "grok", "opencode"] {
            let text = installed(slug, None);
            assert_eq!(
                remove(adapter(slug), Some(&text)),
                Ok(Edit::Delete),
                "{slug}"
            );
        }
    }

    #[test]
    fn gemini_owns_only_its_group() {
        let base = r#"{
  "other": {
    "Stop": [
      {
        "type": "command",
        "command": "notify-me"
      }
    ]
  }
}
"#;
        let after = installed("gemini", Some(base));
        assert!(after.contains("\"other\"") && after.contains("\"cyd-activity\""));
        assert!(after.contains("\"matcher\": \".*\""));
        // Flat events are bare handlers, matcher events are groups.
        let root = json::parse(&after).unwrap();
        let group = root.get("cyd-activity").unwrap();
        assert!(group.get("Stop").unwrap().as_array().unwrap()[0]
            .get("command")
            .is_some());
        assert!(group.get("PreToolUse").unwrap().as_array().unwrap()[0]
            .get("hooks")
            .is_some());
        assert_eq!(
            remove(adapter("gemini"), Some(&after)),
            Ok(Edit::Write(base.to_string()))
        );
    }

    #[test]
    fn gemini_keeps_a_disabled_flag_when_repointing() {
        let text = r#"{"cyd-activity":{"enabled":false,"Stop":[{"type":"command","command":"/old agent-event gemini idle"}]}}"#;
        let after = installed("gemini", Some(text));
        assert!(after.contains("\"enabled\": false"));
        assert_eq!(state("gemini", Some(&after)), HookState::Installed);
    }

    #[test]
    fn grok_file_is_replaced_wholesale() {
        let theirs = r#"{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"stray"}]}]}}"#;
        let text = installed("grok", Some(theirs));
        assert!(!text.contains("stray"));
        let root = json::parse(&text).unwrap();
        let notification = &root
            .get("hooks")
            .unwrap()
            .get("Notification")
            .unwrap()
            .as_array()
            .unwrap()[0];
        assert_eq!(
            notification.get("matcher").and_then(Json::as_str),
            Some("permission_prompt|idle_prompt")
        );
        assert!(!text.contains("timeout"));
    }

    #[test]
    fn claude_tool_groups_carry_the_star_matcher_and_a_timeout() {
        let root = json::parse(&installed("claude", None)).unwrap();
        let hooks = root.get("hooks").unwrap();
        for event in ["PreToolUse", "PostToolUse"] {
            let group = &hooks.get(event).unwrap().as_array().unwrap()[0];
            assert_eq!(group.get("matcher").and_then(Json::as_str), Some("*"));
        }
        let prompt = &hooks.get("UserPromptSubmit").unwrap().as_array().unwrap()[0];
        assert!(prompt.get("matcher").is_none());
        let handler = &prompt.get("hooks").unwrap().as_array().unwrap()[0];
        assert_eq!(handler.get("timeout"), Some(&Json::Num("5".into())));
        assert_eq!(handler.get("type").and_then(Json::as_str), Some("command"));
    }

    #[test]
    fn plugin_template_has_exactly_one_placeholder() {
        assert_eq!(PLUGIN_TEMPLATE.matches(PLUGIN_PLACEHOLDER).count(), 1);
        assert!(plugin_is_ours(PLUGIN_TEMPLATE));
    }

    #[test]
    fn plugin_path_is_js_escaped_and_read_back() {
        let tricky = "/Users/a \"b\"/c\\d\u{2028}/cyd-host";
        let source = plugin_source(tricky);
        assert!(source.contains("\"/Users/a \\\"b\\\"/c\\\\d\\u2028/cyd-host\""));
        assert!(!source.contains(PLUGIN_PLACEHOLDER));
        assert_eq!(
            plugin_path(&source).as_deref(),
            Some("/Users/a \"b\"/c\\d\u{2028}/cyd-host")
        );
    }

    #[test]
    fn plugin_states() {
        let ok = plugin_source(BIN);
        assert_eq!(state("opencode", Some(&ok)), HookState::Installed);
        let moved = plugin_source("/old/cyd-host");
        assert!(
            matches!(state("opencode", Some(&moved)), HookState::Outdated(why) if why.contains("/old/cyd-host"))
        );
        let gone = classify(adapter("opencode"), Some(&ok), BIN, &|_| false);
        assert!(matches!(gone, HookState::Outdated(_)));
        // The unedited template is ours, and outdated.
        assert!(matches!(
            state("opencode", Some(PLUGIN_TEMPLATE)),
            HookState::Outdated(_)
        ));
        assert!(matches!(
            state("opencode", Some("export default {}\n")),
            HookState::Unreadable(_)
        ));
    }

    #[test]
    fn plugin_install_and_remove_refuse_a_foreign_file() {
        let a = adapter("opencode");
        assert!(install(a, Some("export default {}\n"), BIN).is_err());
        assert!(remove(a, Some("export default {}\n")).is_err());
        let ok = plugin_source(BIN);
        assert_eq!(install(a, Some(&ok), BIN), Ok(Edit::Unchanged));
        assert_eq!(
            install(a, Some(&plugin_source("/old")), BIN),
            Ok(Edit::Write(ok.clone()))
        );
        assert_eq!(remove(a, Some(&ok)), Ok(Edit::Delete));
    }

    #[test]
    fn partial_and_outdated_together_report_both() {
        let text = installed("claude", None).replace(BIN, "/old/cyd-host");
        let mut root = json::parse(&text).unwrap();
        root.get_mut("hooks").unwrap().remove("Stop");
        let HookState::Partial(note) = state("claude", Some(&json::pretty(&root))) else {
            panic!("expected partial")
        };
        assert!(
            note.starts_with("missing Stop") && note.contains("outdated"),
            "{note}"
        );
    }
}
