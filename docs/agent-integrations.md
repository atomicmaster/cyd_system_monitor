<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Agent integration boundary

The MVP reports exact same-user process presence for supported agents, and busy and idle only from activity events the agent itself reports through its hooks (below), never from CPU use. A running agent with no event source, or none reported yet, is `running with activity unknown`. V2 may add separately enabled adapters that distinguish busy, idle, and bounded usage through documented local APIs, structured logs, or hooks supplied by the agent.

Each adapter declares the source, fields read, availability states, sampling rate, and retention. It does not inspect private conversation files, prompt or response contents, source contents, command arguments, or credentials. Metric Availability uses the shared available/unsupported/absent/temporarily unavailable/error states. Adapter enablement and Metric Freshness are separate fields: disabled is an adapter setting, and stale describes the age of a retained sample rather than adding another availability enum value. An adapter may report only the evidence its source supports and may not infer semantic task activity from process load alone.

## Process presence: what counts as a supported agent (TB07)

Presence is one question per agent: does a process owned by the current user have a **resolved executable path** that is exactly on the allowlist below? The allowlist is the single table `ALLOWLIST` in `host/metrics/src/agents.rs` (pure, fixture-tested); the macOS collector `host/platform-macos/src/agents.rs` only lists processes with `libproc` (`proc_listallpids`, `proc_pidinfo(PROC_PIDTBSDINFO)` for the owner uid, `proc_pidpath`) and resolves each same-user path with `realpath`.

Privacy rules, each covered by a test or by construction:

- Only the owner uid and the resolved executable path are kept. Command-line arguments, environment, working directory, open files, prompts, conversations, credentials, and process names are never read (the `proc_bsdinfo` name fields are part of the kernel struct and are not looked at). `classify` accepts only `ProcessRecord { uid, path }`.
- Only the current uid counts; another user's process on an allowlisted path is ignored (`another_users_process_on_an_allowlisted_path_does_not_count`).
- Identity is never a bare name (`a_same_named_unrelated_binary_never_counts`).
- A path must be absolute, at most 1024 bytes, without NUL, empty, `.`, `..` or empty segments, or it is rejected, not truncated.
- Version-numbered directories match a strict `N.N.N[-|+tail]` segment (tail at most 32 of `[A-Za-z0-9.-]`), not a wildcard: `latest`, `1.2`, `1.2.3.4` and `..` do not match.
- A process that exits between listing and inspection is skipped; it does not make the source an error. A failed listing, a pid table above 4,096 entries, or a scan over 100 ms is a failed scan: the last good sample is kept with its growing age (the device dims it after 15 s) and after 15 s the source is an error (`--`, `Read error`), never "not running". With no good sample yet (failing from startup) the source is temporarily unavailable, then an error once scans have failed for 15 s. A failed scan is retried at the normal 5 s cadence, never every second.

Allowlist (`~` is the current user's `realpath`ed home; a version segment is `<semver>`). "Verified" rows were resolved on the development Mac by reading symlinks only (no agent was run and no agent file was read); the rest are the documented layouts of the same installers and are unverified there.

| Agent | Resolved path | Why |
| --- | --- | --- |
| Codex | `/opt/homebrew/Caskroom/codex/<semver>/bin/codex` | Verified: `/opt/homebrew/bin/codex` resolves here. Apple Silicon Homebrew cask. |
| Codex | `/usr/local/Caskroom/codex/<semver>/bin/codex` | Intel Homebrew cask prefix. Unverified. |
| Codex | `/opt/homebrew/Cellar/codex/<semver>/bin/codex`, `/usr/local/Cellar/codex/<semver>/bin/codex` | Homebrew formula layout. Unverified. |
| Codex | `~/.vscode/extensions/openai.chatgpt-<semver>-darwin-arm64/bin/macos-aarch64/codex` | Verified: the CLI bundled with OpenAI's VS Code extension, running from there on the development Mac. Apple Silicon only, like the MVP. |
| Codex | `/Applications/Codex.app/Contents/**` | Desktop app bundle root and its helper processes. Not installed on the development Mac; the bundle name is unverified. |
| Claude | `/opt/homebrew/Caskroom/claude-code@latest/<semver>/claude` | Verified: `/opt/homebrew/bin/claude` resolves here. |
| Claude | `/opt/homebrew/Caskroom/claude-code/<semver>/claude` | The versioned cask name. Unverified. |
| Claude | `/usr/local/Caskroom/claude-code@latest/<semver>/claude`, `/usr/local/Caskroom/claude-code/<semver>/claude` | Intel Homebrew prefix. Unverified. |
| Claude | `~/.local/share/claude/versions/<semver>` | Native installer `versions/<semver>` layout that `~/.local/bin/claude` links to. Unverified. |
| Claude | `~/.vscode/extensions/anthropic.claude-code-<semver>-darwin-arm64/resources/native-binary/claude` | Verified: the CLI bundled with the Claude Code VS Code extension, running from there on the development Mac. Apple Silicon only. |
| Claude | `/Applications/Claude.app/Contents/**` | Desktop app bundle root and helpers. Not installed on the development Mac. |
| Grok Build | `~/.grok/bin/grok` | The documented install path; kept for an install that ships a real file there. |
| Grok Build | `~/.grok/downloads/grok-<semver>-macos-aarch64`, `...-macos-x86_64` | Verified: `~/.grok/bin/grok` is a symlink to `../downloads/grok-1.0.30-macos-aarch64`, so a running process resolves to the versioned download. Only under `~/.grok`. |
| Gemini (`agy`) | `~/.local/bin/agy` | Verified: a regular, standalone arm64 Mach-O file on the development Mac (not a symlink), so a running process resolves to this very path. Google's Antigravity CLI, which this project calls Gemini; matched only at this exact home-relative path, never by the bare name `agy`. |
| OpenCode | `/opt/homebrew/Cellar/opencode-v2/<semver>/bin/opencode` | Verified: `/opt/homebrew/bin/opencode` is a symlink to `../Cellar/opencode-v2/2.2.5/bin/opencode`, a standalone arm64 Mach-O. A process launched through the link resolves (`proc_pidpath` and `realpath`) to the versioned Cellar path, the same way Codex resolves to its Caskroom path, so only the Cellar path is listed. Apple Silicon Homebrew. |
| OpenCode | `/usr/local/Cellar/opencode-v2/<semver>/bin/opencode` | Intel Homebrew prefix. Unverified. |

A `Contents/**` row is a bundle root: the path must be `<bundle root>/Contents/<something>`, so a helper counts only inside the bundle (not `/Applications/Claude.app` itself, not a same-named app elsewhere, not a bundle nested inside another app).

T3 Code needs no row of its own: on macOS it launches its configured Claude/Codex binary unchanged (by default the `claude` and `codex` on PATH, the Homebrew casks above), so its sessions count through those paths. The T3 Code app and its helpers are not agents (`t3_code_sessions_count_through_the_binaries_it_launches`). VS Code Insiders, Cursor and other forks keep extensions elsewhere and are not recognised yet.

Deliberately not recognised: `/opt/homebrew/bin/grok` and any other `grok` outside `~/.grok` (Homebrew distributes an unrelated tool under that name: `homebrew_grok_is_not_grok_build`); the unresolved link paths `/opt/homebrew/bin/claude`, `/opt/homebrew/bin/codex` and `/opt/homebrew/bin/opencode` (a process resolves through them to a Caskroom or Cellar path); `agy` or `opencode` at any other path (`/usr/local/bin/agy`, `~/bin/agy`, `~/.local/bin/opencode`: `gemini_and_opencode_never_match_by_name_or_look_alike_path`); npm or other installs that run the agent as a `node` process (identity would be `node`, a bare interpreter name shared with every other program; those Macs show the agent as not running until an accepted adapter or a new allowlist row is added). A cask upgrade that deletes the running version's directory makes that process unresolvable, so it stops counting until restarted.

Cost on the development Mac (437 same-user processes of about 700): a scan takes 6-17 ms (debug build), run inline on the `collect` call at most once every 5 s; the 1 s loop is otherwise a cache read.

## Busy and idle: activity events (every agent)

Busy and idle are never inferred from CPU use. An earlier CPU heuristic (busy at 1% of one core) could not tell an agent waiting on a slow tool or the network from an idle one, and Claude Code's idle CPU sat too close to its working CPU, so it was removed. Instead, each agent's own lifecycle hooks report its state through one shared channel:

- **Command:** `cyd-host agent-event <agent> busy|idle`, where `<agent>` is `codex`, `claude`, `grok`, `gemini` or `opencode`. Anything else is ignored. It drains the hook's stdin unread (the JSON an agent writes there can include the prompt) and always exits 0, so it can never block a tool or a prompt.
- **Identity:** the command walks up its parent processes (at most eight) to the first same-user process whose resolved path is on that agent's allowlist rows. That pid is the only identity used; a hook run by anything else records nothing.
- **Written:** `busy` or `idle` to `<run dir>/agent-events/<agent>-<pid>` (directory `0700`), atomically. Files of processes that no longer exist are removed on each write.
- **Read:** each 5 s scan reads at most 64 files of at most 16 bytes, and only a file whose pid is a running process of that same agent counts. Its age is the file's modification time.
- **States:** busy while any running process of the agent last reported busy; idle when its processes have reported and none is busy; `running, activity unknown` before any report (hooks not installed, or a new session without a prompt yet). A busy report older than 15 minutes with nothing since counts as idle, for a session killed mid-turn.

Each agent's adapter is the set of its hooks that call this command, listed below. An agent with no adapter yet stays `running, activity unknown` while it runs: Grok Build, Gemini and OpenCode, until their sections are added.

## Busy and idle: Claude Code hooks

Claude Code runs a command on lifecycle events. With the hooks below installed in `~/.claude/settings.json`, Claude is **busy** from a prompt until it stops, including while it waits on tools, and **idle** otherwise.

| Event | Records |
| --- | --- |
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse` | busy |
| `Stop`, `Notification` (a permission request or a wait for input), `SessionEnd` | idle |

Each hook runs `cyd-host agent-event claude busy|idle` (the shared channel above). The hook JSON is never read.

## Busy and idle: Codex hooks

Codex supports command hooks in `~/.codex/hooks.json`. This setup targets codex-cli 0.166.0; [0.124.0 made hooks stable](https://github.com/openai/codex/releases/tag/rust-v0.124.0), but the minimum version supporting every event below has not been established. The installed CLI reports hooks as stable and enabled in `codex features list`; `codex --help` describes hook trust and `--no-daemon`. See OpenAI's [hooks documentation](https://developers.openai.com/codex/hooks) and [hook implementation](https://github.com/openai/codex/tree/rust-v0.166.0/codex-rs/hooks).

| Event | Records |
| --- | --- |
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse` | busy |
| `PermissionRequest`, `Stop`, `SessionEnd`, `Interrupt` | idle |

Tool hooks refresh busy when supported tools start or finish. PermissionRequest occurs before approval; automatic approval can therefore briefly report idle. There is no verified event for every wait for user input or for resuming immediately after approval, so idle can persist until the next tool or prompt event. A tool or turn lasting over 15 minutes without another event is shown idle. Before the first event the bar shows activity unknown. Concurrent turns sharing one process can overwrite each other's state.

Merge the following into `~/.codex/hooks.json` (preserve existing hooks; inline `[[hooks.EVENT]]` tables in `~/.codex/config.toml` work too). Replace `/absolute/path/to/cyd-host` with the installed host binary's absolute path; a local debug build is `host/target/debug/cyd-host` under this checkout, after building it. Review and trust the hooks in Codex using `/hooks`.

```json
{
  "hooks": {
    "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex busy"}]}],
    "PreToolUse": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex busy"}]}],
    "PostToolUse": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex busy"}]}],
    "PermissionRequest": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex idle"}]}],
    "Stop": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex idle"}]}],
    "SessionEnd": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex idle"}]}],
    "Interrupt": [{"hooks": [{"type": "command", "command": "/absolute/path/to/cyd-host agent-event codex idle"}]}]
  }
}
```

**Verified on this Mac (codex-cli 0.166.0, `codex exec`):** with `UserPromptSubmit` and `Stop` hooks given inline (`-c 'hooks.UserPromptSubmit=[...]'`, plus `--dangerously-bypass-hook-trust` for that one invocation, with a scratch `CYD_RUN_DIR`), both hooks ran as direct children of the `codex` process (the Homebrew cask binary on the allowlist), and `cyd-host agent-event` wrote `codex-<pid>` as `busy`, then `idle`. A `hooks.json` in an untrusted scratch project did not run: project hooks need a trusted project, and every non-managed hook needs trust (`/hooks`). Not verified: the interactive TUI, `codex app-server` (how T3 Code runs Codex), the daemon `--no-daemon` refers to, and the VS Code extension. If one of them runs hooks from a process that is not a Codex allowlist row within eight parents, nothing is recorded and the bar stays `running, activity unknown`; it never reads busy by mistake.
