// SPDX-License-Identifier: GPL-3.0-only

//! `cyd-host hooks status|install|remove [agent...]`: check, install and
//! remove each agent's activity hooks (`cyd-host agent-event <agent> busy|idle`)
//! so a still mascot can be told apart from "hooks missing".
//!
//! The adapter table and the per-format edits are pure and fixture-tested
//! (`adapters`); file access is thin (`files`); Codex's hook trust goes through
//! its app server (`codex`). Only each agent's one listed config file is read
//! or written, always under the `home` passed in, and never a session, log or
//! credential. See `docs/tickets/TB19.md` and `docs/agent-integrations.md`.

mod adapters;
mod codex;
mod files;
mod json;

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};

use adapters::{adapter_for, classify, install, remove, Adapter, HookState, ADAPTERS};
use codex::{AppServer, CodexRpc};
use metrics::agents::Agent;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Action {
    Status,
    Install,
    Remove,
}

#[derive(Debug)]
struct Args {
    action: Action,
    adapters: Vec<&'static Adapter>,
    binary: Option<String>,
    dry_run: bool,
    trust: bool,
    check: bool,
}

fn parse_args(args: &[String]) -> Result<Args, String> {
    let action = match args.first().map(String::as_str) {
        Some("status") => Action::Status,
        Some("install") => Action::Install,
        Some("remove") => Action::Remove,
        _ => return Err("hooks needs one of status, install, remove".to_string()),
    };
    let mut parsed = Args {
        action,
        adapters: Vec::new(),
        binary: None,
        dry_run: false,
        trust: false,
        check: false,
    };
    let mut rest = args[1..].iter();
    while let Some(arg) = rest.next() {
        match arg.as_str() {
            "--cyd-host" => {
                parsed.binary = Some(rest.next().ok_or("--cyd-host needs a path")?.clone())
            }
            "--dry-run" if action != Action::Status => parsed.dry_run = true,
            "--trust" if action != Action::Remove => parsed.trust = true,
            "--check" if action == Action::Status => parsed.check = true,
            flag if flag.starts_with('-') => {
                return Err(format!("unknown option '{flag}' for hooks {}", args[0]))
            }
            slug => {
                let adapter = adapter_for(slug).ok_or_else(|| {
                    let known: Vec<&str> = ADAPTERS.iter().map(Adapter::slug).collect();
                    format!("unknown agent '{slug}' (one of {})", known.join(", "))
                })?;
                if !parsed.adapters.iter().any(|a| a.slug() == slug) {
                    parsed.adapters.push(adapter);
                }
            }
        }
    }
    if parsed.adapters.is_empty() {
        parsed.adapters = ADAPTERS.iter().collect();
    }
    Ok(parsed)
}

/// One printed result, and whether it counts as fine for the exit status.
#[derive(Debug, PartialEq, Eq)]
struct Outcome {
    slug: &'static str,
    lines: Vec<String>,
    fine: bool,
}

pub struct Context<'a> {
    pub home: &'a Path,
    /// Absolute path of the `cyd-host` the hooks run.
    pub binary: &'a str,
    pub unix_seconds: u64,
    pub codex: &'a dyn CodexRpc,
}

impl Context<'_> {
    fn file(&self, adapter: &Adapter) -> PathBuf {
        self.home.join(adapter.file)
    }

    fn config_dir(&self, adapter: &Adapter) -> PathBuf {
        self.home.join(adapter.config_dir)
    }

    fn agent_installed(&self, adapter: &Adapter) -> bool {
        self.config_dir(adapter).is_dir()
    }
}

fn describe(state: &HookState) -> String {
    match state {
        HookState::Installed => "installed".to_string(),
        HookState::Partial(note) => format!("partial ({note})"),
        HookState::Outdated(note) => format!("outdated ({note})"),
        HookState::NotInstalled => "not installed".to_string(),
        HookState::AgentNotInstalled => "agent not installed".to_string(),
        HookState::Unreadable(why) => format!("config unreadable ({why})"),
    }
}

fn binary_exists(path: &str) -> bool {
    Path::new(path).is_file()
}

/// The state of one agent's hooks on disk.
fn read_state(ctx: &Context, adapter: &Adapter) -> HookState {
    if !ctx.agent_installed(adapter) {
        return HookState::AgentNotInstalled;
    }
    match files::read_config(&ctx.file(adapter)) {
        Ok(text) => classify(adapter, text.as_deref(), ctx.binary, &binary_exists),
        Err(why) => HookState::Unreadable(why),
    }
}

fn trust_line(ctx: &Context, adapter: &Adapter) -> Option<String> {
    (adapter.agent == Agent::Codex).then(|| match ctx.codex.list(ctx.home) {
        Ok(hooks) => format!("trust: {}", codex::describe(&hooks)),
        Err(why) => format!("trust unknown ({why})"),
    })
}

fn status_one(ctx: &Context, adapter: &'static Adapter, trust: bool) -> Outcome {
    let state = read_state(ctx, adapter);
    let mut lines = vec![describe(&state)];
    let hooks_present = !matches!(
        state,
        HookState::NotInstalled | HookState::AgentNotInstalled
    );
    if trust && hooks_present {
        lines.extend(trust_line(ctx, adapter));
    }
    Outcome {
        slug: adapter.slug(),
        lines,
        fine: state.is_fine(),
    }
}

/// Trusts our own Codex hooks, after an install.
fn grant_trust(ctx: &Context) -> String {
    let hooks = match ctx.codex.list(ctx.home) {
        Ok(hooks) => hooks,
        Err(why) => return format!("trust unknown ({why})"),
    };
    let grants = codex::needs_grant(&hooks);
    if grants.is_empty() {
        return format!("trust: {}", codex::describe(&hooks));
    }
    match ctx.codex.grant(ctx.home, &grants) {
        Ok(()) => format!("trusted {} Codex hooks", grants.len()),
        Err(why) => format!("trust unknown ({why})"),
    }
}

fn install_one(ctx: &Context, adapter: &'static Adapter, dry_run: bool, trust: bool) -> Outcome {
    let slug = adapter.slug();
    let done = |lines: Vec<String>, fine: bool| Outcome { slug, lines, fine };
    if !ctx.agent_installed(adapter) {
        return done(vec!["agent not installed; skipped".to_string()], true);
    }
    let file = ctx.file(adapter);
    let existing = match files::read_config(&file) {
        Ok(text) => text,
        Err(why) => {
            return done(
                vec![format!("config unreadable ({why}); left alone")],
                false,
            )
        }
    };
    let edit = match install(adapter, existing.as_deref(), ctx.binary) {
        Ok(edit) => edit,
        Err(why) => {
            return done(
                vec![format!("config unreadable ({why}); left alone")],
                false,
            )
        }
    };
    let mut lines = Vec::new();
    if edit == adapters::Edit::Unchanged {
        lines.push("already installed; nothing changed".to_string());
    } else if dry_run {
        let before = classify(adapter, existing.as_deref(), ctx.binary, &binary_exists);
        lines.push(format!(
            "would write {} (now: {})",
            file.display(),
            describe(&before)
        ));
    } else {
        let stamp = files::timestamp(ctx.unix_seconds);
        match files::apply(&file, &edit, &ctx.config_dir(adapter), &stamp) {
            Ok(applied) => {
                lines.push(format!("installed in {}", file.display()));
                lines.extend(applied.backup.map(|b| format!("backup: {}", b.display())));
            }
            Err(why) => return done(vec![format!("failed: {why}")], false),
        }
    }
    if trust && adapter.agent == Agent::Codex {
        lines.push(if dry_run {
            "would trust the hooks in Codex".to_string()
        } else {
            grant_trust(ctx)
        });
    }
    done(lines, true)
}

fn remove_one(ctx: &Context, adapter: &'static Adapter, dry_run: bool) -> Outcome {
    let slug = adapter.slug();
    let done = |line: String, fine: bool| Outcome {
        slug,
        lines: vec![line],
        fine,
    };
    if !ctx.agent_installed(adapter) {
        return done("agent not installed; nothing to remove".to_string(), true);
    }
    let file = ctx.file(adapter);
    let existing = match files::read_config(&file) {
        Ok(text) => text,
        Err(why) => return done(format!("config unreadable ({why}); left alone"), false),
    };
    let edit = match remove(adapter, existing.as_deref()) {
        Ok(edit) => edit,
        Err(why) => return done(format!("config unreadable ({why}); left alone"), false),
    };
    if edit == adapters::Edit::Unchanged {
        return done("no hooks of ours; nothing changed".to_string(), true);
    }
    if dry_run {
        return done(
            format!("would remove our hooks from {}", file.display()),
            true,
        );
    }
    let stamp = files::timestamp(ctx.unix_seconds);
    match files::apply(&file, &edit, &ctx.config_dir(adapter), &stamp) {
        Ok(applied) => {
            let backup = applied
                .backup
                .map(|b| format!(" (backup: {})", b.display()));
            done(
                format!(
                    "removed our hooks from {}{}",
                    file.display(),
                    backup.unwrap_or_default()
                ),
                true,
            )
        }
        Err(why) => done(format!("failed: {why}"), false),
    }
}

fn execute(args: &Args, ctx: &Context) -> Vec<Outcome> {
    args.adapters
        .iter()
        .map(|&adapter| match args.action {
            Action::Status => status_one(ctx, adapter, args.trust),
            Action::Install => install_one(ctx, adapter, args.dry_run, args.trust),
            Action::Remove => remove_one(ctx, adapter, args.dry_run),
        })
        .collect()
}

/// This executable's own canonical path, or `--cyd-host`.
fn binary_path(requested: Option<&str>) -> Result<String, String> {
    let path = match requested {
        Some(path) => {
            if !Path::new(path).is_absolute() {
                return Err("--cyd-host must be an absolute path".to_string());
            }
            PathBuf::from(path)
        }
        None => std::env::current_exe()
            .and_then(std::fs::canonicalize)
            .map_err(|e| format!("cannot find this program's path: {e}"))?,
    };
    let text = path.to_str().ok_or("the cyd-host path is not UTF-8")?;
    if text.chars().any(char::is_control) {
        return Err("the cyd-host path has a control character".to_string());
    }
    Ok(text.to_string())
}

fn home_dir() -> Result<PathBuf, String> {
    // Not hostcore's `$HOME or /tmp`: editing configs under /tmp would be wrong.
    match std::env::var_os("HOME").map(PathBuf::from) {
        Some(home) if home.is_absolute() => Ok(home),
        _ => Err("HOME is not set to an absolute path".to_string()),
    }
}

/// `cyd-host hooks ...`; `args` starts at `hooks`.
pub fn command(args: &[String]) -> ExitCode {
    let parsed = match parse_args(&args[1..]) {
        Ok(parsed) => parsed,
        Err(why) => {
            eprintln!("error: {why}");
            return ExitCode::FAILURE;
        }
    };
    let (home, binary) = match (home_dir(), binary_path(parsed.binary.as_deref())) {
        (Ok(home), Ok(binary)) => (home, binary),
        (Err(why), _) | (_, Err(why)) => {
            eprintln!("error: {why}");
            return ExitCode::FAILURE;
        }
    };
    let unix_seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    let server = AppServer::default();
    let ctx = Context {
        home: &home,
        binary: &binary,
        unix_seconds,
        codex: &server,
    };
    if parsed.action == Action::Install && !binary_exists(&binary) {
        eprintln!("warning: {binary} does not exist; the hooks will do nothing until it does");
    }
    let outcomes = execute(&parsed, &ctx);
    for outcome in &outcomes {
        let mut lines = outcome.lines.iter();
        if let Some(first) = lines.next() {
            println!("{:<10}{first}", format!("{}:", outcome.slug));
        }
        for line in lines {
            println!("{:<10}{line}", "");
        }
    }
    let all_fine = outcomes.iter().all(|o| o.fine);
    if parsed.action == Action::Status && !all_fine {
        println!("run `cyd-host hooks install` to install or re-point them");
    }
    if all_fine || (parsed.action == Action::Status && !parsed.check) {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

#[cfg(test)]
mod tests;
