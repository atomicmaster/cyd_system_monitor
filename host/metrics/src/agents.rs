// SPDX-License-Identifier: GPL-3.0-only

//! Agent Activity: exact same-user process presence for the supported agents
//! (Codex, Claude, Grok Build). Pure and fixture-tested; the macOS collector
//! only lists processes and resolves their executable paths.
//!
//! Identity is the process's *resolved executable path* matched exactly against
//! the [`ALLOWLIST`] table below, never a bare process name. Nothing here reads
//! or accepts command-line arguments, environment, working directory, open
//! files, prompts, conversations or credentials: a [`ProcessRecord`] carries
//! only the owner uid and the resolved executable path.
//!
//! MVP Hosts only ever produce [`AgentState::NotRunning`] and
//! [`AgentState::RunningActivityUnknown`]; busy and idle need an activity
//! adapter that does not exist yet. The design of each entry, and why, is in
//! `docs/agent-integrations.md`.

/// The supported agents, in the fixed wire slot order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Agent {
    Codex = 0,
    Claude = 1,
    Grok = 2,
}

pub const AGENT_COUNT: usize = 3;
pub const AGENTS: [Agent; AGENT_COUNT] = [Agent::Codex, Agent::Claude, Agent::Grok];

/// Wire values of an agent's state (`protocol/CORE_PROTOCOL.md`, key 8).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum AgentState {
    NotRunning = 0,
    /// A supported process exists; nothing says what it is doing.
    RunningActivityUnknown = 1,
    /// Needs an activity adapter; never produced by process presence.
    Busy = 2,
    /// Needs an activity adapter; never produced by process presence.
    Idle = 3,
}

/// One process as the collector saw it: owner and resolved executable path.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessRecord {
    pub uid: u32,
    pub path: String,
}

/// Longest resolved path considered; anything longer is rejected, not truncated.
pub const MAX_PATH_BYTES: usize = 1024;

/// One path segment of an allowlist pattern.
#[derive(Debug, Clone, Copy)]
enum Seg {
    /// Exactly this text.
    Lit(&'static str),
    /// `<prefix><semver><suffix>`: a version-numbered install directory or file
    /// (`0.155.1`, `1.0.30-macos-aarch64`). The version is `N.N.N`, optionally
    /// followed by `-`/`+` and at most 32 of `[A-Za-z0-9.-]`; nothing else
    /// matches, so it is not a wildcard over arbitrary user paths.
    Versioned(&'static str, &'static str),
}

/// Where an entry is rooted.
#[derive(Debug, Clone, Copy)]
enum Root {
    /// An absolute path.
    Absolute,
    /// Under the current user's resolved home directory.
    Home,
}

#[derive(Debug, Clone, Copy)]
enum Shape {
    /// The whole resolved path is exactly the pattern.
    Executable(&'static [Seg]),
    /// The resolved path is inside `<pattern>/Contents/` (an app bundle root:
    /// the main executable and its helper processes).
    Bundle(&'static [Seg]),
}

#[derive(Debug, Clone, Copy)]
struct Entry {
    agent: Agent,
    root: Root,
    shape: Shape,
}

const fn lit(s: &'static str) -> Seg {
    Seg::Lit(s)
}

/// The allowlist: every resolved path that counts as a supported agent. Each
/// row is explained in `docs/agent-integrations.md`. "Verified" rows were
/// resolved on the development Mac; the rest are the documented layouts of the
/// same installers on other Macs and are unverified here.
const ALLOWLIST: &[Entry] = &[
    // Codex: Homebrew cask (verified: /opt/homebrew/bin/codex ->
    // /opt/homebrew/Caskroom/codex/<semver>/bin/codex), Apple Silicon and Intel.
    Entry {
        agent: Agent::Codex,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("opt"),
            lit("homebrew"),
            lit("Caskroom"),
            lit("codex"),
            Seg::Versioned("", ""),
            lit("bin"),
            lit("codex"),
        ]),
    },
    Entry {
        agent: Agent::Codex,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("usr"),
            lit("local"),
            lit("Caskroom"),
            lit("codex"),
            Seg::Versioned("", ""),
            lit("bin"),
            lit("codex"),
        ]),
    },
    // Codex: Homebrew formula (unverified here).
    Entry {
        agent: Agent::Codex,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("opt"),
            lit("homebrew"),
            lit("Cellar"),
            lit("codex"),
            Seg::Versioned("", ""),
            lit("bin"),
            lit("codex"),
        ]),
    },
    Entry {
        agent: Agent::Codex,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("usr"),
            lit("local"),
            lit("Cellar"),
            lit("codex"),
            Seg::Versioned("", ""),
            lit("bin"),
            lit("codex"),
        ]),
    },
    // Codex: the VS Code extension's bundled CLI (verified), Apple Silicon.
    Entry {
        agent: Agent::Codex,
        root: Root::Home,
        shape: Shape::Executable(&[
            lit(".vscode"),
            lit("extensions"),
            Seg::Versioned("openai.chatgpt-", "-darwin-arm64"),
            lit("bin"),
            lit("macos-aarch64"),
            lit("codex"),
        ]),
    },
    // Codex desktop app bundle and its helpers (unverified: not installed here).
    Entry {
        agent: Agent::Codex,
        root: Root::Absolute,
        shape: Shape::Bundle(&[lit("Applications"), lit("Codex.app")]),
    },
    // Claude: Homebrew cask `claude-code@latest` (verified:
    // /opt/homebrew/bin/claude -> .../Caskroom/claude-code@latest/<semver>/claude)
    // and `claude-code`, Apple Silicon and Intel.
    Entry {
        agent: Agent::Claude,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("opt"),
            lit("homebrew"),
            lit("Caskroom"),
            lit("claude-code@latest"),
            Seg::Versioned("", ""),
            lit("claude"),
        ]),
    },
    Entry {
        agent: Agent::Claude,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("opt"),
            lit("homebrew"),
            lit("Caskroom"),
            lit("claude-code"),
            Seg::Versioned("", ""),
            lit("claude"),
        ]),
    },
    Entry {
        agent: Agent::Claude,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("usr"),
            lit("local"),
            lit("Caskroom"),
            lit("claude-code@latest"),
            Seg::Versioned("", ""),
            lit("claude"),
        ]),
    },
    Entry {
        agent: Agent::Claude,
        root: Root::Absolute,
        shape: Shape::Executable(&[
            lit("usr"),
            lit("local"),
            lit("Caskroom"),
            lit("claude-code"),
            Seg::Versioned("", ""),
            lit("claude"),
        ]),
    },
    // Claude: the native installer's versions/<semver> layout, which
    // ~/.local/bin/claude links to (unverified here).
    Entry {
        agent: Agent::Claude,
        root: Root::Home,
        shape: Shape::Executable(&[
            lit(".local"),
            lit("share"),
            lit("claude"),
            lit("versions"),
            Seg::Versioned("", ""),
        ]),
    },
    // Claude: the VS Code extension's bundled CLI (verified), Apple Silicon.
    Entry {
        agent: Agent::Claude,
        root: Root::Home,
        shape: Shape::Executable(&[
            lit(".vscode"),
            lit("extensions"),
            Seg::Versioned("anthropic.claude-code-", "-darwin-arm64"),
            lit("resources"),
            lit("native-binary"),
            lit("claude"),
        ]),
    },
    // Claude desktop app bundle and its helpers (unverified: not installed here).
    Entry {
        agent: Agent::Claude,
        root: Root::Absolute,
        shape: Shape::Bundle(&[lit("Applications"), lit("Claude.app")]),
    },
    // Grok Build: only under its documented ~/.grok install. `~/.grok/bin/grok`
    // is a symlink to the versioned download (verified), so a running process
    // resolves to the download; the bin path itself is kept for an install that
    // ships a real file there. Homebrew's unrelated `grok` never matches: it is
    // not under ~/.grok.
    Entry {
        agent: Agent::Grok,
        root: Root::Home,
        shape: Shape::Executable(&[lit(".grok"), lit("bin"), lit("grok")]),
    },
    Entry {
        agent: Agent::Grok,
        root: Root::Home,
        shape: Shape::Executable(&[
            lit(".grok"),
            lit("downloads"),
            Seg::Versioned("grok-", "-macos-aarch64"),
        ]),
    },
    Entry {
        agent: Agent::Grok,
        root: Root::Home,
        shape: Shape::Executable(&[
            lit(".grok"),
            lit("downloads"),
            Seg::Versioned("grok-", "-macos-x86_64"),
        ]),
    },
];

/// `N.N.N` with an optional `-`/`+` tail of at most 32 `[A-Za-z0-9.-]`.
fn is_semver(text: &str) -> bool {
    let (core, tail) = match text.find(['-', '+']) {
        Some(at) => (&text[..at], Some(&text[at + 1..])),
        None => (text, None),
    };
    let mut parts = core.split('.');
    let numeric = |p: Option<&str>| {
        p.is_some_and(|p| !p.is_empty() && p.len() <= 9 && p.bytes().all(|b| b.is_ascii_digit()))
    };
    if !(numeric(parts.next()) && numeric(parts.next()) && numeric(parts.next())) {
        return false;
    }
    if parts.next().is_some() {
        return false;
    }
    tail.is_none_or(|t| {
        !t.is_empty()
            && t.len() <= 32
            && t.bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
    })
}

fn segment_matches(seg: &Seg, actual: &str) -> bool {
    match seg {
        Seg::Lit(text) => actual == *text,
        Seg::Versioned(prefix, suffix) => actual
            .strip_prefix(prefix)
            .and_then(|rest| rest.strip_suffix(suffix))
            .is_some_and(is_semver),
    }
}

/// A resolved path is usable only if it is absolute, at most
/// [`MAX_PATH_BYTES`], NUL-free, and has no empty, `.` or `..` segment and no
/// trailing slash: a relative, unnormalised or over-long path is never matched.
pub fn is_resolved_path(path: &str) -> bool {
    if path.len() < 2 || path.len() > MAX_PATH_BYTES || !path.starts_with('/') {
        return false;
    }
    if path.bytes().any(|b| b == 0) {
        return false;
    }
    path[1..]
        .split('/')
        .all(|seg| !seg.is_empty() && seg != "." && seg != "..")
}

/// Splits a valid path (or a valid home) into segments.
fn segments(path: &str) -> Vec<&str> {
    path[1..].split('/').collect()
}

/// Does the resolved `path` (already validated) match `entry` for `home`?
fn entry_matches(entry: &Entry, path_segments: &[&str], home_segments: Option<&[&str]>) -> bool {
    let (pattern, need_contents) = match entry.shape {
        Shape::Executable(p) => (p, false),
        Shape::Bundle(p) => (p, true),
    };
    let mut rest = path_segments;
    if matches!(entry.root, Root::Home) {
        let Some(home) = home_segments else {
            return false;
        };
        if rest.len() < home.len() || rest[..home.len()] != *home {
            return false;
        }
        rest = &rest[home.len()..];
    }
    if rest.len() < pattern.len()
        || !pattern
            .iter()
            .zip(rest)
            .all(|(seg, actual)| segment_matches(seg, actual))
    {
        return false;
    }
    let after = &rest[pattern.len()..];
    if need_contents {
        // Inside `<bundle>/Contents/`, with at least one thing under it.
        after.len() >= 2 && after[0] == "Contents"
    } else {
        after.is_empty()
    }
}

/// The supported agent a resolved executable path belongs to, if any. `home`
/// is the current user's resolved home directory.
pub fn identify(path: &str, home: &str) -> Option<Agent> {
    if !is_resolved_path(path) {
        return None;
    }
    let path_segments = segments(path);
    // A home that is not a normal absolute path (or is `/`) disables Home rows.
    let home_segments = (is_resolved_path(home)).then(|| segments(home));
    ALLOWLIST
        .iter()
        .find(|entry| entry_matches(entry, &path_segments, home_segments.as_deref()))
        .map(|entry| entry.agent)
}

/// Per-agent state from a process listing: an agent is running when at least
/// one process owned by `current_uid` has a resolved path on the allowlist.
/// Duplicates (several processes, helpers) still count once. Records of other
/// users and non-matching paths are ignored.
pub fn classify(
    records: &[ProcessRecord],
    current_uid: u32,
    home: &str,
) -> [AgentState; AGENT_COUNT] {
    let mut state = [AgentState::NotRunning; AGENT_COUNT];
    for record in records {
        if record.uid != current_uid {
            continue;
        }
        if let Some(agent) = identify(&record.path, home) {
            state[agent as usize] = AgentState::RunningActivityUnknown;
        }
    }
    state
}

#[cfg(test)]
mod tests {
    use super::*;

    const HOME: &str = "/Users/tester";
    const UID: u32 = 501;

    fn rec(uid: u32, path: &str) -> ProcessRecord {
        ProcessRecord {
            uid,
            path: path.to_string(),
        }
    }

    fn states(records: &[ProcessRecord]) -> [u8; 3] {
        classify(records, UID, HOME).map(|s| s as u8)
    }

    #[test]
    fn the_verified_install_paths_of_each_agent_match() {
        assert_eq!(
            identify("/opt/homebrew/Caskroom/codex/0.155.1/bin/codex", HOME),
            Some(Agent::Codex)
        );
        assert_eq!(
            identify(
                "/opt/homebrew/Caskroom/claude-code@latest/2.1.280/claude",
                HOME
            ),
            Some(Agent::Claude)
        );
        assert_eq!(
            identify(
                "/Users/tester/.grok/downloads/grok-1.0.30-macos-aarch64",
                HOME
            ),
            Some(Agent::Grok)
        );
        assert_eq!(
            identify("/Users/tester/.grok/bin/grok", HOME),
            Some(Agent::Grok)
        );
        assert_eq!(
            identify("/Users/tester/.local/share/claude/versions/2.1.3", HOME),
            Some(Agent::Claude)
        );
    }

    #[test]
    fn the_vs_code_extension_binaries_match() {
        assert_eq!(
            identify(
                "/Users/tester/.vscode/extensions/anthropic.claude-code-2.1.296-darwin-arm64/resources/native-binary/claude",
                HOME
            ),
            Some(Agent::Claude)
        );
        assert_eq!(
            identify(
                "/Users/tester/.vscode/extensions/openai.chatgpt-26.1007.21434-darwin-arm64/bin/macos-aarch64/codex",
                HOME
            ),
            Some(Agent::Codex)
        );
    }

    #[test]
    fn vs_code_rows_need_the_exact_extension_layout() {
        for path in [
            // Another user's extensions, or outside ~/.vscode.
            "/Users/other/.vscode/extensions/anthropic.claude-code-2.1.296-darwin-arm64/resources/native-binary/claude",
            "/Users/tester/vscode/extensions/anthropic.claude-code-2.1.296-darwin-arm64/resources/native-binary/claude",
            // A look-alike publisher, an unversioned or Intel directory.
            "/Users/tester/.vscode/extensions/evil.claude-code-2.1.296-darwin-arm64/resources/native-binary/claude",
            "/Users/tester/.vscode/extensions/anthropic.claude-code-latest-darwin-arm64/resources/native-binary/claude",
            "/Users/tester/.vscode/extensions/anthropic.claude-code-2.1.296-darwin-x64/resources/native-binary/claude",
            // The right directory but another file in it.
            "/Users/tester/.vscode/extensions/anthropic.claude-code-2.1.296-darwin-arm64/resources/native-binary/node",
            "/Users/tester/.vscode/extensions/openai.chatgpt-26.1007.21434-darwin-arm64/bin/macos-aarch64/rg",
            "/Users/tester/.vscode/extensions/openai.chatgpt-26.1007.21434-darwin-arm64/bin/codex",
        ] {
            assert_eq!(states(&[rec(UID, path)]), [0, 0, 0], "{path}");
        }
    }

    #[test]
    fn t3_code_sessions_count_through_the_binaries_it_launches() {
        // T3 Code launches the configured `claude`/`codex` (by default the ones
        // on PATH) unchanged; the collector sees their resolved paths. The app
        // itself and its helpers are not agents.
        assert_eq!(
            states(&[
                rec(UID, "/Applications/T3 Code (Alpha).app/Contents/MacOS/T3 Code (Alpha)"),
                rec(UID, "/Applications/T3 Code (Alpha).app/Contents/Resources/resource-monitor/t3-resource-monitor"),
            ]),
            [0, 0, 0]
        );
        assert_eq!(
            states(&[
                rec(
                    UID,
                    "/Applications/T3 Code (Alpha).app/Contents/MacOS/T3 Code (Alpha)"
                ),
                rec(
                    UID,
                    "/opt/homebrew/Caskroom/claude-code@latest/2.1.296/claude"
                ),
                rec(UID, "/opt/homebrew/Caskroom/codex/0.162.1/bin/codex"),
            ]),
            [1, 1, 0]
        );
    }

    #[test]
    fn version_directories_survive_upgrades_but_are_not_wildcards() {
        for ok in ["0.156.0", "1.2.3-beta.1", "10.20.30+build.5"] {
            let path = format!("/opt/homebrew/Caskroom/codex/{ok}/bin/codex");
            assert_eq!(identify(&path, HOME), Some(Agent::Codex), "{ok}");
        }
        for bad in [
            "latest",
            "1.2",
            "1.2.3.4",
            "x.y.z",
            "",
            "1.2.3-",
            "1.2.3/../9",
        ] {
            let path = format!("/opt/homebrew/Caskroom/codex/{bad}/bin/codex");
            assert_eq!(identify(&path, HOME), None, "{bad:?}");
        }
        // The tail is bounded.
        let long = format!(
            "/opt/homebrew/Caskroom/codex/1.2.3-{}/bin/codex",
            "a".repeat(33)
        );
        assert_eq!(identify(&long, HOME), None);
    }

    #[test]
    fn a_same_named_unrelated_binary_never_counts() {
        for path in [
            "/usr/local/bin/claude",
            "/opt/homebrew/bin/claude",
            "/opt/homebrew/bin/codex",
            "/tmp/claude",
            "/Users/tester/bin/codex",
            "/Users/tester/Downloads/claude/claude",
            "/opt/homebrew/Caskroom/codex/0.155.1/codex", // wrong file under the right root
            "/opt/homebrew/Caskroom/other/0.155.1/bin/codex",
            "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex-helper",
            "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex/extra",
            "/opt/homebrew/Caskroom/claude-code@latest/2.1.280/claude-other",
        ] {
            assert_eq!(states(&[rec(UID, path)]), [0, 0, 0], "{path}");
        }
    }

    #[test]
    fn homebrew_grok_is_not_grok_build() {
        for path in [
            "/opt/homebrew/bin/grok",
            "/opt/homebrew/Cellar/grok/3.0.4/bin/grok",
            "/usr/local/bin/grok",
            "/Users/tester/bin/grok",
            "/Users/tester/.grok/other/grok",
            "/Users/tester/.grok/downloads/grok",
            "/Users/tester/.grok/downloads/grok-1.0.30-linux-x86_64",
            "/Users/tester/.grok/downloads/grok-latest-macos-aarch64",
            "/Users/tester/.grok/bin/grok/child",
            "/Users/other/.grok/bin/grok",
        ] {
            assert_eq!(states(&[rec(UID, path)]), [0, 0, 0], "{path}");
        }
        assert_eq!(
            states(&[rec(UID, "/Users/tester/.grok/bin/grok")]),
            [0, 0, 1]
        );
    }

    #[test]
    fn another_users_process_on_an_allowlisted_path_does_not_count() {
        let path = "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex";
        assert_eq!(states(&[rec(UID + 1, path)]), [0, 0, 0]);
        assert_eq!(states(&[rec(0, path)]), [0, 0, 0]);
        assert_eq!(states(&[rec(UID + 1, path), rec(UID, path)]), [1, 0, 0]);
    }

    #[test]
    fn a_symlink_already_resolved_into_the_allowlist_matches_only_when_resolved() {
        // The collector resolves symlinks; the classifier sees the target.
        assert_eq!(
            states(&[rec(UID, "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex")]),
            [1, 0, 0]
        );
        // The unresolved link path is not on the allowlist.
        assert_eq!(states(&[rec(UID, "/opt/homebrew/bin/codex")]), [0, 0, 0]);
    }

    #[test]
    fn malformed_relative_and_overlong_paths_are_rejected() {
        let good = "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex";
        assert_eq!(identify(good, HOME), Some(Agent::Codex));
        for path in [
            "",
            "/",
            "codex",
            "opt/homebrew/Caskroom/codex/0.155.1/bin/codex",
            "./opt/homebrew/Caskroom/codex/0.155.1/bin/codex",
            "/opt/homebrew/Caskroom/codex/0.155.1/../0.155.1/bin/codex",
            "/opt/homebrew/Caskroom/codex/./0.155.1/bin/codex",
            "/opt/homebrew//Caskroom/codex/0.155.1/bin/codex",
            "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex/",
            "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex\0",
            "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex\0/x",
            "/Users/tester/../tester/.grok/bin/grok",
        ] {
            assert_eq!(identify(path, HOME), None, "{path:?}");
        }
        let exactly = format!("/{}", "a".repeat(MAX_PATH_BYTES - 1));
        assert!(is_resolved_path(&exactly));
        let over = format!("/{}", "a".repeat(MAX_PATH_BYTES));
        assert!(!is_resolved_path(&over));
        // A long path that still starts like an allowlisted one is rejected.
        let padded = format!("{good}/{}", "a".repeat(MAX_PATH_BYTES));
        assert_eq!(identify(&padded, HOME), None);
    }

    #[test]
    fn a_bad_home_disables_home_rows_only() {
        for home in ["", "/", "relative", "/Users/../Users/x", "/Users/x/"] {
            assert_eq!(
                identify("/Users/tester/.grok/bin/grok", home),
                None,
                "{home:?}"
            );
            assert_eq!(
                identify("/opt/homebrew/Caskroom/codex/0.155.1/bin/codex", home),
                Some(Agent::Codex)
            );
        }
        // Home is a whole-segment prefix, not a string prefix.
        assert_eq!(identify("/Users/testerx/.grok/bin/grok", HOME), None);
    }

    #[test]
    fn bundle_helpers_count_only_inside_the_bundle_root() {
        assert_eq!(
            states(&[rec(UID, "/Applications/Claude.app/Contents/MacOS/Claude")]),
            [0, 1, 0]
        );
        assert_eq!(
            states(&[rec(
                UID,
                "/Applications/Claude.app/Contents/Frameworks/Claude Helper.app/Contents/MacOS/Claude Helper"
            )]),
            [0, 1, 0]
        );
        assert_eq!(
            states(&[rec(UID, "/Applications/Codex.app/Contents/MacOS/Codex")]),
            [1, 0, 0]
        );
        for path in [
            "/Applications/Claude.app",
            "/Applications/Claude.app/Contents",
            "/Applications/Claude.app/MacOS/Claude", // not under Contents
            "/Applications/Claude.app.zip/Contents/MacOS/x",
            "/Applications/Claude Helper.app/Contents/MacOS/x",
            "/Applications/Other.app/Contents/Frameworks/Claude.app/Contents/MacOS/Claude",
            "/Users/tester/Downloads/Claude.app/Contents/MacOS/Claude",
            "/Applications/Claude.app/Contents/../../../tmp/evil",
        ] {
            assert_eq!(states(&[rec(UID, path)]), [0, 0, 0], "{path}");
        }
    }

    #[test]
    fn duplicates_count_once_and_agents_are_independent() {
        let codex = "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex";
        let claude = "/opt/homebrew/Caskroom/claude-code@latest/2.1.280/claude";
        assert_eq!(
            states(&[rec(UID, codex), rec(UID, codex), rec(UID, codex)]),
            [1, 0, 0]
        );
        assert_eq!(
            states(&[
                rec(UID, claude),
                rec(UID, "/usr/bin/zsh"),
                rec(UID, codex),
                rec(UID, claude),
            ]),
            [1, 1, 0]
        );
    }

    #[test]
    fn zero_processes_is_all_not_running() {
        assert_eq!(states(&[]), [0, 0, 0]);
    }

    #[test]
    fn presence_never_yields_busy_or_idle() {
        let records = [
            rec(UID, "/opt/homebrew/Caskroom/codex/0.155.1/bin/codex"),
            rec(UID, "/Users/tester/.grok/bin/grok"),
        ];
        for state in classify(&records, UID, HOME) {
            assert!(matches!(
                state,
                AgentState::NotRunning | AgentState::RunningActivityUnknown
            ));
        }
    }

    #[test]
    fn the_allowlist_is_small_and_every_agent_has_an_entry() {
        assert!(ALLOWLIST.len() <= 16);
        for agent in AGENTS {
            assert!(ALLOWLIST.iter().any(|e| e.agent == agent));
        }
    }
}
