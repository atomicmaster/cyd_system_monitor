<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Agent integration boundary

The MVP reports exact same-user process presence for supported agents and uses `running with activity unknown` unless a trustworthy adapter supplies stronger evidence. V2 may add separately enabled adapters that distinguish busy, idle, and bounded usage through documented local APIs, structured logs, or hooks supplied by the agent.

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
| Codex | `/Applications/Codex.app/Contents/**` | Desktop app bundle root and its helper processes. Not installed on the development Mac; the bundle name is unverified. |
| Claude | `/opt/homebrew/Caskroom/claude-code@latest/<semver>/claude` | Verified: `/opt/homebrew/bin/claude` resolves here. |
| Claude | `/opt/homebrew/Caskroom/claude-code/<semver>/claude` | The versioned cask name. Unverified. |
| Claude | `/usr/local/Caskroom/claude-code@latest/<semver>/claude`, `/usr/local/Caskroom/claude-code/<semver>/claude` | Intel Homebrew prefix. Unverified. |
| Claude | `~/.local/share/claude/versions/<semver>` | Native installer `versions/<semver>` layout that `~/.local/bin/claude` links to. Unverified. |
| Claude | `/Applications/Claude.app/Contents/**` | Desktop app bundle root and helpers. Not installed on the development Mac. |
| Grok Build | `~/.grok/bin/grok` | The documented install path; kept for an install that ships a real file there. |
| Grok Build | `~/.grok/downloads/grok-<semver>-macos-aarch64`, `...-macos-x86_64` | Verified: `~/.grok/bin/grok` is a symlink to `../downloads/grok-1.0.30-macos-aarch64`, so a running process resolves to the versioned download. Only under `~/.grok`. |

A `Contents/**` row is a bundle root: the path must be `<bundle root>/Contents/<something>`, so a helper counts only inside the bundle (not `/Applications/Claude.app` itself, not a same-named app elsewhere, not a bundle nested inside another app).

Deliberately not recognised: `/opt/homebrew/bin/grok` and any other `grok` outside `~/.grok` (Homebrew distributes an unrelated tool under that name: `homebrew_grok_is_not_grok_build`); the unresolved link paths `/opt/homebrew/bin/claude` and `/opt/homebrew/bin/codex` (a process resolves through them to a Caskroom path); npm or other installs that run the agent as a `node` process (identity would be `node`, a bare interpreter name shared with every other program; those Macs show the agent as not running until an accepted adapter or a new allowlist row is added). A cask upgrade that deletes the running version's directory makes that process unresolvable, so it stops counting until restarted.

Cost on the development Mac (437 same-user processes of about 700): a scan takes 6-17 ms (debug build), run inline on the `collect` call at most once every 5 s; the 1 s loop is otherwise a cache read.

Without an activity adapter a running agent is `running, activity unknown` (wire state 1); busy (2) and idle (3) exist on the wire and on the card for a future adapter but the MVP Host never sends them.

