// SPDX-License-Identifier: GPL-3.0-only

//! Codex runs only hooks it has been told to trust. Its app server reports
//! each hook's trust (`hooks/list`) and records trust (`config/batchWrite` on
//! `hooks.state`), the same calls its `/hooks` screen makes, so neither the
//! check nor the grant reimplements Codex's hash. The server is a child
//! process speaking newline-delimited JSON-RPC on stdio, behind
//! [`CodexRpc`] so everything else is tested with a fake. Every failure is
//! "trust unknown"; nothing here can block or break an install.

use std::ffi::OsString;
use std::io::{BufRead, BufReader, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

use super::json::{self, Json};

/// The whole exchange with `codex app-server` is bounded to this long.
pub const TIMEOUT: Duration = Duration::from_secs(15);
const MAX_REPLY_BYTES: usize = 4 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodexHook {
    pub key: String,
    /// `trusted`, `untrusted`, `modified` or `managed`.
    pub trust_status: String,
    pub current_hash: String,
    /// Its command is one of ours (`agent-event codex `).
    pub ours: bool,
}

pub trait CodexRpc {
    /// Every hook Codex loads for sessions started in `home`.
    fn list(&self, home: &Path) -> Result<Vec<CodexHook>, String>;
    /// Records trust for `(key, hash)` pairs.
    fn grant(&self, home: &Path, grants: &[(String, String)]) -> Result<(), String>;
}

/// Hooks of ours that Codex would not run, as `(key, current hash)`.
pub fn needs_grant(hooks: &[CodexHook]) -> Vec<(String, String)> {
    hooks
        .iter()
        .filter(|h| h.ours && matches!(h.trust_status.as_str(), "untrusted" | "modified"))
        .filter(|h| !h.current_hash.is_empty())
        .map(|h| (h.key.clone(), h.current_hash.clone()))
        .collect()
}

/// One line of status text for the trust of our hooks.
pub fn describe(hooks: &[CodexHook]) -> String {
    let ours: Vec<&CodexHook> = hooks.iter().filter(|h| h.ours).collect();
    if ours.is_empty() {
        return "no cyd-host hooks loaded by Codex".to_string();
    }
    let count = |status: &str| ours.iter().filter(|h| h.trust_status == status).count();
    let unrun = ours.len() - count("trusted") - count("managed");
    if unrun == 0 {
        return format!("all {} hooks trusted", ours.len());
    }
    let mut parts = Vec::new();
    for status in ["untrusted", "modified"] {
        if count(status) > 0 {
            parts.push(format!("{} {status}", count(status)));
        }
    }
    if parts.is_empty() {
        parts.push(format!("{unrun} in an unknown state"));
    }
    format!(
        "{} of {} hooks not trusted ({}); `cyd-host hooks install codex --trust` trusts them",
        unrun,
        ours.len(),
        parts.join(", ")
    )
}

/// The real thing: `codex app-server`.
pub struct AppServer {
    pub program: OsString,
    pub timeout: Duration,
}

impl Default for AppServer {
    fn default() -> Self {
        AppServer {
            program: "codex".into(),
            timeout: TIMEOUT,
        }
    }
}

struct Session {
    child: Child,
    stdin: ChildStdin,
    lines: Receiver<String>,
    deadline: Instant,
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn object(fields: Vec<(&str, Json)>) -> Json {
    Json::Obj(
        fields
            .into_iter()
            .map(|(k, v)| (k.to_string(), v))
            .collect(),
    )
}

impl Session {
    fn start(server: &AppServer) -> Result<Session, String> {
        let mut child = Command::new(&server.program)
            .arg("app-server")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|e| format!("cannot run codex: {e}"))?;
        let (Some(stdin), Some(stdout)) = (child.stdin.take(), child.stdout.take()) else {
            let _ = child.kill();
            return Err("cannot talk to codex".to_string());
        };
        let (tx, lines) = mpsc::channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                if tx.send(line).is_err() {
                    break;
                }
            }
        });
        let mut session = Session {
            child,
            stdin,
            lines,
            deadline: Instant::now() + server.timeout,
        };
        let info = object(vec![
            ("name", Json::str("cyd-host")),
            ("version", Json::str(env!("CARGO_PKG_VERSION"))),
        ]);
        session.request(0, "initialize", object(vec![("clientInfo", info)]))?;
        session.send(&object(vec![("method", Json::str("initialized"))]))?;
        Ok(session)
    }

    fn send(&mut self, message: &Json) -> Result<(), String> {
        let line = format!("{}\n", json::compact(message));
        self.stdin
            .write_all(line.as_bytes())
            .and_then(|()| self.stdin.flush())
            .map_err(|e| format!("codex closed its input: {e}"))
    }

    /// Sends a request and returns its `result`, skipping notifications.
    fn request(&mut self, id: u32, method: &str, params: Json) -> Result<Json, String> {
        self.send(&object(vec![
            ("id", Json::Num(id.to_string())),
            ("method", Json::str(method)),
            ("params", params),
        ]))?;
        loop {
            let left = self.deadline.saturating_duration_since(Instant::now());
            let line = match self.lines.recv_timeout(left) {
                Ok(line) => line,
                Err(RecvTimeoutError::Timeout) => return Err("codex did not answer in time".into()),
                Err(RecvTimeoutError::Disconnected) => return Err("codex exited".into()),
            };
            let Ok(message) = json::parse_limited(&line, MAX_REPLY_BYTES) else {
                continue;
            };
            if message.get("id") != Some(&Json::Num(id.to_string())) {
                continue;
            }
            if let Some(error) = message.get("error") {
                let why = error
                    .get("message")
                    .and_then(Json::as_str)
                    .unwrap_or("error");
                return Err(format!("codex {method}: {why}"));
            }
            return message
                .get("result")
                .cloned()
                .ok_or_else(|| format!("codex {method}: no result"));
        }
    }
}

fn home_text(home: &Path) -> Result<&str, String> {
    home.to_str().ok_or_else(|| "home is not UTF-8".to_string())
}

/// `result.data[0].hooks[]` of a `hooks/list` reply.
fn parse_hooks(result: &Json) -> Result<Vec<CodexHook>, String> {
    let hooks = result
        .get("data")
        .and_then(Json::as_array)
        .and_then(|data| data.first())
        .and_then(|first| first.get("hooks"))
        .and_then(Json::as_array)
        .ok_or("unexpected hooks/list reply")?;
    Ok(hooks
        .iter()
        .filter_map(|hook| {
            Some(CodexHook {
                key: hook.get("key")?.as_str()?.to_string(),
                trust_status: hook.get("trustStatus")?.as_str()?.to_string(),
                current_hash: hook
                    .get("currentHash")
                    .and_then(Json::as_str)
                    .unwrap_or_default()
                    .to_string(),
                ours: json::compact(hook).contains("agent-event codex "),
            })
        })
        .collect())
}

impl CodexRpc for AppServer {
    fn list(&self, home: &Path) -> Result<Vec<CodexHook>, String> {
        let mut session = Session::start(self)?;
        let cwds = Json::Arr(vec![Json::str(home_text(home)?)]);
        let result = session.request(1, "hooks/list", object(vec![("cwds", cwds)]))?;
        parse_hooks(&result)
    }

    fn grant(&self, home: &Path, grants: &[(String, String)]) -> Result<(), String> {
        home_text(home)?;
        let mut session = Session::start(self)?;
        let state = Json::Obj(
            grants
                .iter()
                .map(|(key, hash)| (key.clone(), object(vec![("trusted_hash", Json::str(hash))])))
                .collect(),
        );
        let edit = object(vec![
            ("keyPath", Json::str("hooks.state")),
            ("value", state),
            ("mergeStrategy", Json::str("upsert")),
        ]);
        let params = object(vec![
            ("edits", Json::Arr(vec![edit])),
            ("filePath", Json::Null),
            ("expectedVersion", Json::Null),
            ("reloadUserConfig", Json::Bool(true)),
        ]);
        session.request(2, "config/batchWrite", params).map(|_| ())
    }
}

#[cfg(test)]
pub mod tests {
    use std::cell::RefCell;
    use std::os::unix::fs::PermissionsExt;

    use super::*;

    pub fn hook(key: &str, status: &str, ours: bool) -> CodexHook {
        CodexHook {
            key: key.to_string(),
            trust_status: status.to_string(),
            current_hash: format!("sha256:{key}"),
            ours,
        }
    }

    /// A fake Codex: what it lists, and the grants it was given.
    pub struct Fake {
        pub hooks: Result<Vec<CodexHook>, String>,
        pub granted: RefCell<Vec<(String, String)>>,
    }

    impl CodexRpc for Fake {
        fn list(&self, _: &Path) -> Result<Vec<CodexHook>, String> {
            self.hooks.clone()
        }

        fn grant(&self, _: &Path, grants: &[(String, String)]) -> Result<(), String> {
            self.granted.borrow_mut().extend_from_slice(grants);
            Ok(())
        }
    }

    #[test]
    fn only_our_untrusted_or_modified_hooks_need_a_grant() {
        let hooks = vec![
            hook("a", "trusted", true),
            hook("b", "untrusted", true),
            hook("c", "modified", true),
            hook("d", "untrusted", false),
            hook("e", "managed", true),
        ];
        assert_eq!(
            needs_grant(&hooks),
            [
                ("b".to_string(), "sha256:b".to_string()),
                ("c".to_string(), "sha256:c".to_string())
            ]
        );
        let mut empty_hash = hook("f", "untrusted", true);
        empty_hash.current_hash.clear();
        assert!(needs_grant(&[empty_hash]).is_empty());
    }

    #[test]
    fn trust_is_described_for_our_hooks_only() {
        assert_eq!(describe(&[]), "no cyd-host hooks loaded by Codex");
        assert_eq!(
            describe(&[hook("x", "untrusted", false)]),
            "no cyd-host hooks loaded by Codex"
        );
        assert_eq!(
            describe(&[
                hook("a", "trusted", true),
                hook("b", "managed", true),
                hook("c", "untrusted", false)
            ]),
            "all 2 hooks trusted"
        );
        let text = describe(&[
            hook("a", "trusted", true),
            hook("b", "untrusted", true),
            hook("c", "modified", true),
        ]);
        assert!(
            text.starts_with("2 of 3 hooks not trusted (1 untrusted, 1 modified)"),
            "{text}"
        );
    }

    #[test]
    fn a_hooks_list_reply_is_read() {
        let reply = json::parse(
            r#"{"data":[{"cwd":"/h","hooks":[
              {"key":"/h/.codex/hooks.json:stop:0:0","trustStatus":"untrusted","currentHash":"sha256:1","command":"'/b' agent-event codex idle 2>/dev/null || true"},
              {"key":"other","trustStatus":"trusted","currentHash":"sha256:2","command":"herdr"},
              {"trustStatus":"trusted"}
            ]}]}"#,
        )
        .unwrap();
        let hooks = parse_hooks(&reply).unwrap();
        assert_eq!(hooks.len(), 2);
        assert!(hooks[0].ours && !hooks[1].ours);
        assert_eq!(hooks[0].current_hash, "sha256:1");
        assert!(parse_hooks(&json::parse("{}").unwrap()).is_err());
        assert!(parse_hooks(&json::parse(r#"{"data":[]}"#).unwrap()).is_err());
    }

    fn scratch(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("cyd-codex-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn script(dir: &Path, body: &str) -> OsString {
        let path = dir.join("codex");
        std::fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
        path.into()
    }

    #[test]
    fn the_app_server_exchange_lists_and_grants() {
        let dir = scratch("exchange");
        // A fake `codex app-server`: answers initialize, ignores the
        // notification, then answers whatever single request follows.
        let program = script(
            &dir,
            r#"read init; echo '{"jsonrpc":"2.0","method":"noise","params":{}}'
echo '{"id":0,"result":{"userAgent":"fake"}}'
read initialized
read request
case "$request" in
  *hooks/list*) echo '{"id":1,"result":{"data":[{"hooks":[{"key":"k1","trustStatus":"untrusted","currentHash":"h1","command":"x agent-event codex busy"}]}]}}' ;;
  *config/batchWrite*) echo "$request" > "$(dirname "$0")/write.txt"; echo '{"id":2,"result":{"status":"ok"}}' ;;
esac"#,
        );
        let server = AppServer {
            program,
            timeout: Duration::from_secs(10),
        };
        let hooks = server.list(Path::new("/h")).unwrap();
        assert_eq!(
            hooks,
            [CodexHook {
                key: "k1".into(),
                trust_status: "untrusted".into(),
                current_hash: "h1".into(),
                ours: true,
            }]
        );
        server
            .grant(Path::new("/h"), &[("k1".into(), "h1".into())])
            .unwrap();
        let sent = std::fs::read_to_string(dir.join("write.txt")).unwrap();
        assert!(sent.contains("\"keyPath\":\"hooks.state\""), "{sent}");
        assert!(
            sent.contains("{\"k1\":{\"trusted_hash\":\"h1\"}}"),
            "{sent}"
        );
        assert!(
            sent.contains("\"mergeStrategy\":\"upsert\"")
                && sent.contains("\"reloadUserConfig\":true"),
            "{sent}"
        );
        assert!(
            sent.contains("\"filePath\":null") && sent.contains("\"expectedVersion\":null"),
            "{sent}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_silent_codex_times_out_and_is_killed() {
        let dir = scratch("silent");
        let program = script(&dir, "echo $$ > \"$(dirname \"$0\")/pid.txt\"; sleep 30");
        let server = AppServer {
            program,
            timeout: Duration::from_millis(300),
        };
        let started = Instant::now();
        let error = server.list(Path::new("/h")).unwrap_err();
        assert!(error.contains("in time"), "{error}");
        assert!(started.elapsed() < Duration::from_secs(5));
        let pid: i32 = std::fs::read_to_string(dir.join("pid.txt"))
            .unwrap()
            .trim()
            .parse()
            .unwrap();
        std::thread::sleep(Duration::from_millis(200));
        let alive = Command::new("kill")
            .args(["-0", &pid.to_string()])
            .stderr(Stdio::null())
            .status()
            .unwrap()
            .success();
        assert!(!alive, "the child was not killed");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn errors_exits_and_a_missing_program_are_reported_not_raised() {
        let dir = scratch("errors");
        let server = AppServer {
            program: script(
                &dir,
                "read l; echo '{\"id\":0,\"error\":{\"message\":\"nope\"}}'",
            ),
            timeout: Duration::from_secs(5),
        };
        assert!(server.list(Path::new("/h")).unwrap_err().contains("nope"));
        let server = AppServer {
            program: script(&dir, "exit 3"),
            timeout: Duration::from_secs(5),
        };
        assert!(server.list(Path::new("/h")).is_err());
        let server = AppServer {
            program: dir.join("absent").into(),
            timeout: Duration::from_secs(5),
        };
        assert!(server
            .list(Path::new("/h"))
            .unwrap_err()
            .contains("cannot run codex"));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
