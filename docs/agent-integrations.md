<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Agent integration boundary

The MVP reports exact same-user process presence for supported agents, plus a CPU heuristic for busy and idle (below); `running with activity unknown` remains for a process with nothing to compare against. V2 may add separately enabled adapters that distinguish busy, idle, and bounded usage through documented local APIs, structured logs, or hooks supplied by the agent.

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

A `Contents/**` row is a bundle root: the path must be `<bundle root>/Contents/<something>`, so a helper counts only inside the bundle (not `/Applications/Claude.app` itself, not a same-named app elsewhere, not a bundle nested inside another app).

T3 Code needs no row of its own: on macOS it launches its configured Claude/Codex binary unchanged (by default the `claude` and `codex` on PATH, the Homebrew casks above), so its sessions count through those paths. The T3 Code app and its helpers are not agents (`t3_code_sessions_count_through_the_binaries_it_launches`). VS Code Insiders, Cursor and other forks keep extensions elsewhere and are not recognised yet.

Deliberately not recognised: `/opt/homebrew/bin/grok` and any other `grok` outside `~/.grok` (Homebrew distributes an unrelated tool under that name: `homebrew_grok_is_not_grok_build`); the unresolved link paths `/opt/homebrew/bin/claude` and `/opt/homebrew/bin/codex` (a process resolves through them to a Caskroom path); npm or other installs that run the agent as a `node` process (identity would be `node`, a bare interpreter name shared with every other program; those Macs show the agent as not running until an accepted adapter or a new allowlist row is added). A cask upgrade that deletes the running version's directory makes that process unresolvable, so it stops counting until restarted.

Cost on the development Mac (437 same-user processes of about 700): a scan takes 6-17 ms (debug build), run inline on the `collect` call at most once every 5 s; the 1 s loop is otherwise a cache read.

## Busy and idle: the CPU heuristic (Codex, Grok Build)

A running Codex or Grok Build is **busy** when its allowlisted processes together used at least 1% of one core (`BUSY_CPU_PERMILLE`) since the previous scan, and **idle** below that. With nothing to compare against (the first scan, after a wake, or only new pids) it stays `running, activity unknown` (wire state 1). Busy is an inference from CPU use, not a report from the agent: an agent waiting quietly on a slow tool or the network can read idle, and one running a heavy tool in a child process is not counted (only the allowlisted process itself is measured).

- **Source:** `proc_pidinfo(PROC_PIDTASKINFO)`, user + system CPU time, read only for processes already matched by the allowlist above.
- **Fields read:** those two counters and the pid. Nothing else from the task info is used.
- **Sampling:** the same 5 s scan as presence; the threshold is relative to the real time between scans.
- **Retention:** only the previous scan's counters per pid, in memory; dropped on wake, so time asleep never reads as idle.
- **Measured** on the development Mac over 20 s: idle Codex 0%, five idle Grok processes 0.3% together, a working Claude Code waiting on a tool 2.2%. Claude Code is excluded: its idle CPU (about 1%) sits too close to its working CPU (2-4% while waiting on a tool), so the crab danced while idle. Claude uses its hooks instead (below).

## Busy and idle: Claude Code hooks

Claude Code runs a command on lifecycle events. With the hooks below installed in `~/.claude/settings.json`, Claude is **busy** from a prompt until it stops, including while it waits on tools, and **idle** otherwise.

| Event | Records |
| --- | --- |
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse` | busy |
| `Stop`, `Notification` (a permission request or a wait for input), `SessionEnd` | idle |

Each hook runs `cyd-host agent-event claude busy|idle`:

- **Source:** the hook command, run by Claude Code itself. It finds the Claude process that ran it by walking up its parent processes (at most eight) to the first same-user process whose resolved path is on the Claude allowlist. That pid is the only identity used.
- **Fields read:** none from the hook. The JSON Claude Code writes to the hook's stdin (which can include the prompt) is drained unread, so the writer never sees a broken pipe.
- **Written:** `busy` or `idle` to `<run dir>/agent-events/claude-<pid>` (directory `0700`), atomically. Files of Claude processes that no longer exist are removed on each write. The command always exits 0, so it can never block a tool or a prompt.
- **Read:** each 5 s scan reads at most 64 files of at most 16 bytes, and only files whose pid is a running Claude process count. Their age is the file's modification time.
- **States:** busy while any running Claude process last reported busy; idle when its processes have reported and none is busy; `running, activity unknown` before any report (hooks not installed, or a new session without a prompt yet). A busy report older than 15 minutes with nothing since counts as idle, for a session killed mid-turn.

