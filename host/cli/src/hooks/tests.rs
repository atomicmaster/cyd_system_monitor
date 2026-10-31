// SPDX-License-Identifier: GPL-3.0-only

//! End-to-end tests over a fake home: every adapter against its config
//! fixture, install/remove round trips, idempotence, dry runs, status exit
//! behaviour and the Codex trust flow with a fake app server.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

use super::adapters::{self, plugin_source, Edit, Format, HookState, ADAPTERS};
use super::codex::tests::{hook, Fake};
use super::*;

const BIN: &str = "/opt/cyd/cyd-host";

fn fixture(name: &str) -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("fixtures/hooks")
        .join(name);
    fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn extension(adapter: &Adapter) -> &str {
    adapter.file.rsplit('.').next().unwrap_or("json")
}

fn present(_: &str) -> bool {
    true
}

/// Writes the golden file instead of comparing when `UPDATE_FIXTURES` is set.
fn golden(name: &str, actual: &str) {
    if std::env::var_os("UPDATE_FIXTURES").is_some() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("fixtures/hooks")
            .join(name);
        fs::write(path, actual).unwrap();
    }
    assert_eq!(actual, fixture(name), "{name}");
}

#[test]
fn every_adapter_has_a_config_fixture_and_round_trips_through_it() {
    for adapter in ADAPTERS {
        let slug = adapter.slug();
        let ext = extension(adapter);
        let foreign = fixture(&format!("{slug}.foreign.{ext}"));
        match adapter.format {
            Format::Plugin => {
                // A foreign file under our name is refused, never replaced.
                assert!(matches!(
                    classify(adapter, Some(&foreign), BIN, &present),
                    HookState::Unreadable(_)
                ));
                assert!(adapters::install(adapter, Some(&foreign), BIN).is_err());
                golden(&format!("{slug}.installed.{ext}"), &plugin_source(BIN));
            }
            Format::Json { .. } => {
                let Ok(Edit::Write(installed)) = adapters::install(adapter, Some(&foreign), BIN)
                else {
                    panic!("{slug}: install did not write")
                };
                golden(&format!("{slug}.installed.{ext}"), &installed);
                assert_eq!(
                    classify(adapter, Some(&installed), BIN, &present),
                    HookState::Installed,
                    "{slug}"
                );
                assert_eq!(
                    adapters::install(adapter, Some(&installed), BIN),
                    Ok(Edit::Unchanged),
                    "{slug}"
                );
                // Foreign hooks survive an install.
                for word in ["herdr", "tldraw", "notify", "audit-log"] {
                    assert_eq!(
                        foreign.contains(word),
                        installed.contains(word),
                        "{slug} {word}"
                    );
                }
                // Install then remove restores the file, except an adapter that
                // owns the whole file (it is replaced and deleted as one).
                let removed = adapters::remove(adapter, Some(&installed)).unwrap();
                match adapter.format {
                    Format::Json {
                        scope: adapters::Scope::WholeFile,
                        ..
                    } => {
                        assert_eq!(removed, Edit::Delete, "{slug}")
                    }
                    _ => assert_eq!(removed, Edit::Write(foreign.clone()), "{slug}"),
                }
            }
        }
    }
}

#[test]
fn a_status_of_every_kind_for_every_json_adapter() {
    for adapter in ADAPTERS
        .iter()
        .filter(|a| matches!(a.format, Format::Json { .. }))
    {
        let slug = adapter.slug();
        let ext = extension(adapter);
        let installed = fixture(&format!("{slug}.installed.{ext}"));
        let state = |text: Option<&str>| classify(adapter, text, BIN, &present);
        assert_eq!(state(Some(&installed)), HookState::Installed, "{slug}");
        assert_eq!(state(None), HookState::NotInstalled, "{slug}");
        assert!(
            matches!(
                state(Some(&installed.replace(BIN, "/moved/cyd-host"))),
                HookState::Outdated(_)
            ),
            "{slug}"
        );
        assert!(
            matches!(
                classify(adapter, Some(&installed), BIN, &|_| false),
                HookState::Outdated(_)
            ),
            "{slug}"
        );
        assert!(
            matches!(state(Some("{ not json")), HookState::Unreadable(_)),
            "{slug}"
        );
        // A partial config: drop the last event of ours.
        let mut root = super::json::parse(&installed).unwrap();
        let Format::Json { key, .. } = adapter.format else {
            unreachable!()
        };
        let last = adapter.events.last().unwrap().event;
        root.get_mut(key).unwrap().remove(last);
        assert!(
            matches!(state(Some(&super::json::pretty(&root))), HookState::Partial(note) if note.contains(last)),
            "{slug}"
        );
        // Other people's hooks alone do not count as ours.
        let foreign = fixture(&format!("{slug}.foreign.{ext}"));
        if !foreign.contains("agent-event") {
            assert_eq!(state(Some(&foreign)), HookState::NotInstalled, "{slug}");
        }
    }
}

/// A fake home with the given agents' config directories.
struct Home {
    dir: PathBuf,
}

impl Home {
    fn new(name: &str, agents: &[&str]) -> Home {
        let dir = std::env::temp_dir().join(format!("cyd-hooks-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        for slug in agents {
            fs::create_dir_all(dir.join(adapters::adapter_for(slug).unwrap().config_dir)).unwrap();
        }
        Home { dir }
    }

    fn write(&self, rel: &str, text: &str) {
        let path = self.dir.join(rel);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    fn read(&self, rel: &str) -> Option<String> {
        fs::read_to_string(self.dir.join(rel)).ok()
    }

    /// Every file under the home with its contents and mode.
    fn snapshot(&self) -> BTreeMap<String, (Vec<u8>, u32)> {
        fn walk(dir: &Path, root: &Path, out: &mut BTreeMap<String, (Vec<u8>, u32)>) {
            for entry in fs::read_dir(dir).unwrap() {
                let path = entry.unwrap().path();
                if path.is_dir() {
                    walk(&path, root, out);
                } else {
                    let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o7777;
                    out.insert(
                        path.strip_prefix(root).unwrap().display().to_string(),
                        (fs::read(&path).unwrap(), mode),
                    );
                }
            }
        }
        let mut out = BTreeMap::new();
        walk(&self.dir, &self.dir, &mut out);
        out
    }

    fn backups(&self) -> Vec<String> {
        self.snapshot()
            .into_keys()
            .filter(|k| k.contains(".bak-cyd-"))
            .collect()
    }
}

impl Drop for Home {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.dir);
    }
}

fn run(home: &Home, args: &[&str], codex: &dyn CodexRpc) -> Vec<Outcome> {
    let args: Vec<String> = args.iter().map(|a| a.to_string()).collect();
    let parsed = parse_args(&args).unwrap();
    // 2026-07-01 00:00:00 UTC.
    let ctx = Context {
        home: &home.dir,
        binary: BIN,
        unix_seconds: 1_782_864_000,
        codex,
    };
    execute(&parsed, &ctx)
}

fn no_codex() -> Fake {
    Fake {
        hooks: Err("codex is not available in this test".into()),
        granted: RefCell::new(vec![]),
    }
}

fn outcome<'a>(all: &'a [Outcome], slug: &str) -> &'a Outcome {
    all.iter().find(|o| o.slug == slug).unwrap()
}

/// `outcome.lines[0]` after the slug.
fn first(all: &[Outcome], slug: &str) -> String {
    outcome(all, slug).lines[0].clone()
}

#[test]
fn install_all_then_status_all_then_remove_all_in_a_fake_home() {
    let home = Home::new("all", &["claude", "codex", "gemini", "opencode"]);
    home.write(".claude/settings.json", &fixture("claude.foreign.json"));
    home.write(".codex/hooks.json", &fixture("codex.foreign.json"));
    let before = home.snapshot();
    let codex = no_codex();

    let status = run(&home, &["status"], &codex);
    assert_eq!(first(&status, "claude"), "not installed");
    assert_eq!(first(&status, "grok"), "agent not installed");
    assert_eq!(first(&status, "opencode"), "not installed");
    assert_eq!(home.snapshot(), before, "status writes nothing");

    let installed = run(&home, &["install"], &codex);
    assert!(installed.iter().all(|o| o.fine));
    assert_eq!(first(&installed, "grok"), "agent not installed; skipped");
    assert!(
        !home.dir.join(".grok").exists(),
        "no directory for an agent that is not installed"
    );
    assert!(home
        .read(".claude/settings.json")
        .unwrap()
        .contains("herdr agent report"));
    assert!(home
        .read(".gemini/config/hooks.json")
        .unwrap()
        .contains("cyd-activity"));
    assert!(home
        .read(".config/opencode/plugins/cyd-system-monitor.js")
        .is_some());
    assert_eq!(
        home.backups().len(),
        2,
        "only the two files that existed are backed up"
    );
    assert_eq!(
        fs::metadata(home.dir.join(".gemini/config"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o700
    );

    // Nothing at BIN exists here, so everything reads as outdated, not missing.
    let status = run(&home, &["status"], &codex);
    for slug in ["claude", "codex", "gemini", "opencode"] {
        assert!(
            first(&status, slug).starts_with("outdated"),
            "{slug}: {}",
            first(&status, slug)
        );
    }

    let after = home.snapshot();
    let again = run(&home, &["install"], &codex);
    assert!(again.iter().all(|o| o.fine));
    assert_eq!(
        first(&again, "claude"),
        "already installed; nothing changed"
    );
    assert_eq!(
        home.snapshot(),
        after,
        "a second install changes nothing and makes no backup"
    );

    let removed = run(&home, &["remove"], &codex);
    assert!(removed.iter().all(|o| o.fine));
    assert_eq!(
        home.read(".claude/settings.json"),
        Some(fixture("claude.foreign.json"))
    );
    assert_eq!(
        home.read(".codex/hooks.json"),
        Some(fixture("codex.foreign.json"))
    );
    assert_eq!(home.read(".gemini/config/hooks.json"), None);
    assert_eq!(
        home.read(".config/opencode/plugins/cyd-system-monitor.js"),
        None
    );
    let again = run(&home, &["remove"], &codex);
    assert_eq!(first(&again, "claude"), "no hooks of ours; nothing changed");
}

#[test]
fn a_real_binary_makes_status_installed() {
    let home = Home::new("real", &["claude"]);
    let binary = home.dir.join("cyd-host");
    fs::write(&binary, "").unwrap();
    let path = binary.to_str().unwrap();
    let args: Vec<String> = ["install", "claude"].map(String::from).into();
    let codex = no_codex();
    let ctx = Context {
        home: &home.dir,
        binary: path,
        unix_seconds: 0,
        codex: &codex,
    };
    execute(&parse_args(&args).unwrap(), &ctx);
    let status: Vec<String> = ["status", "claude"].map(String::from).into();
    let outcomes = execute(&parse_args(&status).unwrap(), &ctx);
    assert_eq!(first(&outcomes, "claude"), "installed");
    assert!(outcomes[0].fine);
}

#[test]
fn a_dry_run_changes_nothing() {
    let home = Home::new("dry", &["claude", "opencode"]);
    home.write(".claude/settings.json", &fixture("claude.foreign.json"));
    let before = home.snapshot();
    let codex = no_codex();
    let out = run(&home, &["install", "--dry-run"], &codex);
    assert!(first(&out, "claude").starts_with("would write "));
    assert!(first(&out, "opencode").starts_with("would write "));
    assert_eq!(home.snapshot(), before);
    run(&home, &["install", "claude"], &codex);
    let before = home.snapshot();
    let out = run(&home, &["remove", "--dry-run"], &codex);
    assert!(first(&out, "claude").starts_with("would remove "));
    assert_eq!(home.snapshot(), before);
}

#[test]
fn unreadable_configs_are_reported_and_left_untouched() {
    let home = Home::new("bad", &["claude", "codex", "opencode"]);
    home.write(
        ".claude/settings.json",
        "{\n  // a comment\n  \"hooks\": {}\n}\n",
    );
    home.write(".codex/hooks.json", "{\"hooks\": [");
    home.write(
        ".config/opencode/plugins/cyd-system-monitor.js",
        &fixture("opencode.foreign.js"),
    );
    let before = home.snapshot();
    let codex = no_codex();
    for args in [&["install"][..], &["remove"][..]] {
        let out = run(&home, args, &codex);
        for slug in ["claude", "codex", "opencode"] {
            assert!(
                first(&out, slug).starts_with("config unreadable"),
                "{slug}: {}",
                first(&out, slug)
            );
            assert!(!outcome(&out, slug).fine);
        }
        assert_eq!(home.snapshot(), before);
    }
    let out = run(&home, &["status"], &codex);
    assert!(first(&out, "claude").starts_with("config unreadable"));
    assert!(first(&out, "claude").contains("comments"));
}

#[test]
fn a_missing_agent_directory_is_not_an_error() {
    let home = Home::new("none", &[]);
    let codex = no_codex();
    for action in ["status", "install", "remove"] {
        let out = run(&home, &[action], &codex);
        assert!(out.iter().all(|o| o.fine), "{action}");
        assert_eq!(
            fs::read_dir(&home.dir).unwrap().count(),
            0,
            "{action} created something"
        );
    }
}

#[test]
fn only_the_named_agents_are_touched() {
    let home = Home::new("named", &["claude", "codex"]);
    let codex = no_codex();
    let out = run(&home, &["install", "claude"], &codex);
    assert_eq!(out.len(), 1);
    assert!(home.read(".claude/settings.json").is_some());
    assert!(home.read(".codex/hooks.json").is_none());
}

#[test]
fn status_exit_follows_check() {
    let all_fine = |outcomes: &[Outcome]| outcomes.iter().all(|o| o.fine);
    let home = Home::new("check", &["claude"]);
    let codex = no_codex();
    assert!(
        !all_fine(&run(&home, &["status", "claude"], &codex)),
        "not installed is not fine"
    );
    // `--check` is what turns that into a failing exit status.
    assert!(
        parse_args(&["status".into(), "--check".into()])
            .unwrap()
            .check
    );
    assert!(!parse_args(&["status".into()]).unwrap().check);
    let none = Home::new("check-none", &[]);
    assert!(
        all_fine(&run(&none, &["status"], &codex)),
        "agents that are not installed are fine"
    );
}

#[test]
fn arguments_are_validated() {
    let parse = |args: &[&str]| parse_args(&args.iter().map(|a| a.to_string()).collect::<Vec<_>>());
    assert!(parse(&[]).is_err());
    assert!(parse(&["fix"]).is_err());
    assert!(parse(&["install", "cursor"])
        .unwrap_err()
        .contains("codex, claude"));
    assert!(parse(&["install", "--bogus"]).is_err());
    assert!(parse(&["install", "--check"]).is_err());
    assert!(parse(&["status", "--dry-run"]).is_err());
    assert!(parse(&["remove", "--trust"]).is_err());
    assert!(parse(&["install", "--cyd-host"]).is_err());
    let args = parse(&[
        "install",
        "codex",
        "claude",
        "codex",
        "--cyd-host",
        "/x/cyd-host",
        "--trust",
        "--dry-run",
    ])
    .unwrap();
    assert_eq!(
        args.adapters.iter().map(|a| a.slug()).collect::<Vec<_>>(),
        ["codex", "claude"]
    );
    assert_eq!(args.binary.as_deref(), Some("/x/cyd-host"));
    assert!(args.trust && args.dry_run);
    assert_eq!(parse(&["status"]).unwrap().adapters.len(), ADAPTERS.len());
}

#[test]
fn the_binary_path_is_checked() {
    assert_eq!(binary_path(Some("/a b/cyd-host")).unwrap(), "/a b/cyd-host");
    assert!(binary_path(Some("relative/cyd-host")).is_err());
    assert!(binary_path(Some("/a\nb")).is_err());
    let own = binary_path(None).unwrap();
    assert!(Path::new(&own).is_absolute());
    assert_eq!(fs::canonicalize(&own).unwrap(), PathBuf::from(&own));
}

#[test]
fn codex_trust_is_shown_only_when_asked_and_granted_only_for_ours() {
    let home = Home::new("trust", &["codex"]);
    let silent = no_codex();
    run(&home, &["install", "codex"], &silent);
    let fake = Fake {
        hooks: Ok(vec![
            hook("ours-1", "untrusted", true),
            hook("ours-2", "trusted", true),
            hook("ours-3", "modified", true),
            hook("herdr-1", "untrusted", false),
        ]),
        granted: RefCell::new(vec![]),
    };
    // Without --trust, status never asks Codex.
    let out = run(&home, &["status", "codex"], &fake);
    assert_eq!(out[0].lines.len(), 1);
    let out = run(&home, &["status", "codex", "--trust"], &fake);
    assert!(
        out[0].lines[1].starts_with("trust: 2 of 3 hooks not trusted"),
        "{:?}",
        out[0].lines
    );
    assert!(fake.granted.borrow().is_empty(), "status never grants");

    let out = run(&home, &["install", "codex", "--trust"], &fake);
    assert_eq!(out[0].lines.last().unwrap(), "trusted 2 Codex hooks");
    let granted = fake.granted.borrow().clone();
    assert_eq!(
        granted,
        [
            ("ours-1".to_string(), "sha256:ours-1".to_string()),
            ("ours-3".to_string(), "sha256:ours-3".to_string())
        ],
        "foreign and already trusted hooks are left alone"
    );
    // Without --trust, install grants nothing.
    fake.granted.borrow_mut().clear();
    run(&home, &["install", "codex"], &fake);
    assert!(fake.granted.borrow().is_empty());
    let dry = run(&home, &["install", "codex", "--trust", "--dry-run"], &fake);
    assert!(dry[0].lines.last().unwrap().starts_with("would trust"));
    assert!(fake.granted.borrow().is_empty());
}

#[test]
fn codex_trust_failures_read_as_unknown_and_never_fail_the_install() {
    let home = Home::new("trust-fail", &["codex"]);
    let broken = no_codex();
    let out = run(&home, &["install", "codex", "--trust"], &broken);
    assert!(out[0].fine);
    assert!(
        out[0].lines.last().unwrap().starts_with("trust unknown ("),
        "{:?}",
        out[0].lines
    );
    let out = run(&home, &["status", "codex", "--trust"], &broken);
    assert!(out[0].lines[1].starts_with("trust unknown ("));
    // No hooks installed: nothing to be trusted, so Codex is not asked.
    let empty = Home::new("trust-none", &["codex"]);
    let out = run(&empty, &["status", "codex", "--trust"], &broken);
    assert_eq!(out[0].lines, ["not installed"]);
}

#[test]
fn nothing_but_the_listed_files_is_read() {
    // A planted sibling that is not a listed config must never be needed.
    let home = Home::new("scope", &["claude"]);
    home.write(".claude/projects/session.jsonl", "secret prompt");
    home.write(".claude/.credentials.json", "{\"token\": \"secret\"}");
    let codex = no_codex();
    run(&home, &["install", "claude"], &codex);
    run(&home, &["remove", "claude"], &codex);
    assert_eq!(
        home.read(".claude/projects/session.jsonl").as_deref(),
        Some("secret prompt")
    );
    assert_eq!(
        home.read(".claude/.credentials.json").as_deref(),
        Some("{\"token\": \"secret\"}")
    );
}
