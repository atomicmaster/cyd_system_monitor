// SPDX-License-Identifier: GPL-3.0-only

//! Per-user LaunchAgent lifecycle for `cyd-hostd`: generate the plist, and
//! drive it with `launchctl bootstrap/bootout/kickstart/print` in the caller's
//! `gui/<uid>` domain. `launchctl` sits behind a trait so tests (and
//! `--dry-run`) never touch the real launchd, and the LaunchAgents directory
//! is a parameter so tests use a temp directory. No root is involved.
//!
//! Uninstall removes only the plist and the launchd registration; pairing
//! identity, histories and logs are never touched here (erase is a separate,
//! later action).

use std::fs;
use std::io::{self, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::Command;

use hostcore::paths::SERVICE_LABEL;

/// Which launchd domain and label an operation targets.
pub struct ServiceConfig {
    pub label: String,
    pub daemon_path: PathBuf,
    pub launch_agents_dir: PathBuf,
    pub log_dir: PathBuf,
    pub uid: u32,
}

impl ServiceConfig {
    pub fn plist_path(&self) -> PathBuf {
        self.launch_agents_dir.join(format!("{}.plist", self.label))
    }

    fn domain(&self) -> String {
        format!("gui/{}", self.uid)
    }

    fn target(&self) -> String {
        format!("gui/{}/{}", self.uid, self.label)
    }
}

pub fn default_label() -> String {
    SERVICE_LABEL.to_string()
}

fn xml(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

/// The LaunchAgent property list. `KeepAlive/SuccessfulExit=false` restarts
/// the daemon after a crash or failure but not after a clean stop (exit 0);
/// `Adaptive` lets launchd raise the priority while the app is interacted
/// with rather than pinning a once-per-second sampler to background QoS.
pub fn render_plist(config: &ServiceConfig) -> String {
    let log = xml(&config.log_dir.join("hostd-launchd.log").to_string_lossy());
    format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
<plist version=\"1.0\">\n\
<dict>\n\
\t<key>Label</key>\n\
\t<string>{label}</string>\n\
\t<key>ProgramArguments</key>\n\
\t<array>\n\
\t\t<string>{daemon}</string>\n\
\t\t<string>--log-dir</string>\n\
\t\t<string>{logs}</string>\n\
\t</array>\n\
\t<key>RunAtLoad</key>\n\
\t<true/>\n\
\t<key>KeepAlive</key>\n\
\t<dict>\n\
\t\t<key>SuccessfulExit</key>\n\
\t\t<false/>\n\
\t</dict>\n\
\t<key>ProcessType</key>\n\
\t<string>Adaptive</string>\n\
\t<key>ThrottleInterval</key>\n\
\t<integer>10</integer>\n\
\t<key>LimitLoadToSessionType</key>\n\
\t<string>Aqua</string>\n\
\t<key>StandardOutPath</key>\n\
\t<string>{log}</string>\n\
\t<key>StandardErrorPath</key>\n\
\t<string>{log}</string>\n\
</dict>\n\
</plist>\n",
        label = xml(&config.label),
        daemon = xml(&config.daemon_path.to_string_lossy()),
        logs = xml(&config.log_dir.to_string_lossy()),
    )
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaunchctlOutput {
    pub code: i32,
    pub stdout: String,
}

pub trait Launchctl {
    fn run(&self, args: &[String]) -> io::Result<LaunchctlOutput>;
}

pub struct SystemLaunchctl;

impl Launchctl for SystemLaunchctl {
    fn run(&self, args: &[String]) -> io::Result<LaunchctlOutput> {
        let out = Command::new("/bin/launchctl").args(args).output()?;
        Ok(LaunchctlOutput {
            code: out.status.code().unwrap_or(-1),
            stdout: String::from_utf8_lossy(&out.stdout).into_owned()
                + &String::from_utf8_lossy(&out.stderr),
        })
    }
}

/// Runs nothing: records the commands that would run and reports "not loaded".
pub struct DryRunLaunchctl {
    pub commands: std::cell::RefCell<Vec<String>>,
}

impl Launchctl for DryRunLaunchctl {
    fn run(&self, args: &[String]) -> io::Result<LaunchctlOutput> {
        if args.first().map(String::as_str) == Some("print") {
            return Ok(LaunchctlOutput {
                code: 113,
                stdout: String::new(),
            });
        }
        self.commands
            .borrow_mut()
            .push(format!("launchctl {}", args.join(" ")));
        Ok(LaunchctlOutput {
            code: 0,
            stdout: String::new(),
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    Install { load: bool },
    Uninstall,
    Start,
    Stop,
    Restart,
    Status,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ServiceStatus {
    pub installed: bool,
    pub loaded: bool,
    pub running: bool,
    pub pid: Option<u32>,
}

impl ServiceStatus {
    pub fn describe(&self) -> String {
        format!(
            "installed: {}\nloaded: {}\nrunning: {}\npid: {}",
            yes(self.installed),
            yes(self.loaded),
            yes(self.running),
            self.pid.map_or("none".into(), |p| p.to_string())
        )
    }
}

fn yes(v: bool) -> &'static str {
    if v {
        "yes"
    } else {
        "no"
    }
}

/// Reads `state = running` and `pid = N` from the top level (one tab of
/// indent) of `launchctl print` output.
pub fn parse_print(output: &str) -> (bool, Option<u32>) {
    let mut running = false;
    let mut pid = None;
    for line in output.lines() {
        if let Some(rest) = line.strip_prefix('\t') {
            if rest.starts_with('\t') {
                continue; // nested (arguments, environment, endpoints)
            }
            if let Some(state) = rest.strip_prefix("state = ") {
                running = state.trim() == "running";
            } else if let Some(p) = rest.strip_prefix("pid = ") {
                pid = p.trim().parse().ok();
            }
        }
    }
    (running, pid)
}

pub struct Report {
    /// What was done or would be done, one line each.
    pub lines: Vec<String>,
    pub status: Option<ServiceStatus>,
}

fn args(list: &[&str]) -> Vec<String> {
    list.iter().map(|s| s.to_string()).collect()
}

fn query(config: &ServiceConfig, ctl: &dyn Launchctl) -> Result<ServiceStatus, String> {
    let out = ctl
        .run(&args(&["print", &config.target()]))
        .map_err(|e| format!("cannot run launchctl: {e}"))?;
    let loaded = out.code == 0;
    let (running, pid) = if loaded {
        parse_print(&out.stdout)
    } else {
        (false, None)
    };
    Ok(ServiceStatus {
        installed: config.plist_path().is_file(),
        loaded,
        running,
        pid,
    })
}

fn launchctl(ctl: &dyn Launchctl, list: &[&str], lines: &mut Vec<String>) -> Result<(), String> {
    let out = ctl
        .run(&args(list))
        .map_err(|e| format!("cannot run launchctl: {e}"))?;
    lines.push(format!("launchctl {}", list.join(" ")));
    if out.code != 0 {
        return Err(format!(
            "launchctl {} failed ({}): {}",
            list.join(" "),
            out.code,
            out.stdout.trim()
        ));
    }
    Ok(())
}

fn write_plist(config: &ServiceConfig, lines: &mut Vec<String>) -> Result<(), String> {
    let io_err = |what: &str, e: io::Error| format!("{what}: {e}");
    fs::create_dir_all(&config.launch_agents_dir)
        .map_err(|e| io_err("cannot create the LaunchAgents directory", e))?;
    fs::create_dir_all(&config.log_dir)
        .map_err(|e| io_err("cannot create the log directory", e))?;
    fs::set_permissions(&config.log_dir, fs::Permissions::from_mode(0o700))
        .map_err(|e| io_err("cannot restrict the log directory", e))?;
    let path = config.plist_path();
    let tmp = path.with_extension("plist.tmp");
    let mut file = fs::File::create(&tmp).map_err(|e| io_err("cannot write the plist", e))?;
    file.write_all(render_plist(config).as_bytes())
        .and_then(|()| file.sync_all())
        .map_err(|e| io_err("cannot write the plist", e))?;
    fs::set_permissions(&tmp, fs::Permissions::from_mode(0o644))
        .map_err(|e| io_err("cannot set the plist mode", e))?;
    fs::rename(&tmp, &path).map_err(|e| io_err("cannot install the plist", e))?;
    lines.push(format!("wrote {}", path.display()));
    Ok(())
}

/// Performs `action`. With `dry_run` nothing on disk changes and `ctl` should
/// be a [`DryRunLaunchctl`]; the report lists what would happen.
pub fn perform(
    config: &ServiceConfig,
    ctl: &dyn Launchctl,
    action: Action,
    dry_run: bool,
) -> Result<Report, String> {
    let mut lines = Vec::new();
    let before = query(config, ctl)?;
    match action {
        Action::Status => {
            return Ok(Report {
                lines,
                status: Some(before),
            })
        }
        Action::Install { load } => {
            if !dry_run {
                let meta = fs::metadata(&config.daemon_path).map_err(|e| {
                    format!(
                        "cannot find the daemon at {}: {e}",
                        config.daemon_path.display()
                    )
                })?;
                if !meta.is_file() || meta.permissions().mode() & 0o111 == 0 {
                    return Err(format!(
                        "{} is not an executable file",
                        config.daemon_path.display()
                    ));
                }
                write_plist(config, &mut lines)?;
            } else {
                lines.push(format!("would write {}:", config.plist_path().display()));
                lines.push(render_plist(config));
            }
            if load {
                if before.loaded {
                    launchctl(ctl, &["bootout", &config.target()], &mut lines)?;
                }
                let plist = config.plist_path();
                launchctl(
                    ctl,
                    &["bootstrap", &config.domain(), &plist.to_string_lossy()],
                    &mut lines,
                )?;
            }
        }
        Action::Uninstall => {
            if before.loaded {
                launchctl(ctl, &["bootout", &config.target()], &mut lines)?;
            }
            if before.installed && !dry_run {
                fs::remove_file(config.plist_path())
                    .map_err(|e| format!("cannot remove the plist: {e}"))?;
                lines.push(format!("removed {}", config.plist_path().display()));
            } else if before.installed {
                lines.push(format!("would remove {}", config.plist_path().display()));
            }
            lines.push(
                "kept: pairing identity, histories and logs (erasing them is a separate action)"
                    .into(),
            );
        }
        Action::Start => {
            if !before.installed && !dry_run {
                return Err("the service is not installed; run `cyd-host service install`".into());
            }
            if !before.loaded {
                let plist = config.plist_path();
                launchctl(
                    ctl,
                    &["bootstrap", &config.domain(), &plist.to_string_lossy()],
                    &mut lines,
                )?;
            } else if !before.running {
                launchctl(ctl, &["kickstart", &config.target()], &mut lines)?;
            } else {
                lines.push("already running".into());
            }
        }
        Action::Stop => {
            if before.loaded {
                launchctl(ctl, &["bootout", &config.target()], &mut lines)?;
                lines.push("stopped until `service start` or the next login".into());
            } else {
                lines.push("already stopped".into());
            }
        }
        Action::Restart => {
            if !before.installed && !dry_run {
                return Err("the service is not installed; run `cyd-host service install`".into());
            }
            if before.loaded {
                launchctl(ctl, &["kickstart", "-k", &config.target()], &mut lines)?;
            } else {
                let plist = config.plist_path();
                launchctl(
                    ctl,
                    &["bootstrap", &config.domain(), &plist.to_string_lossy()],
                    &mut lines,
                )?;
            }
        }
    }
    let status = if dry_run {
        None
    } else {
        Some(query(config, ctl)?)
    };
    Ok(Report { lines, status })
}

/// The daemon sits next to the CLI unless told otherwise.
pub fn default_daemon_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|dir| dir.join("cyd-hostd")))
        .unwrap_or_else(|| PathBuf::from("cyd-hostd"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::path::Path;

    /// A launchctl that models one job: loaded/running state changes with the
    /// verbs, and every call is recorded.
    struct Fake {
        loaded: RefCell<bool>,
        running: RefCell<bool>,
        calls: RefCell<Vec<String>>,
    }

    impl Fake {
        fn new() -> Self {
            Fake {
                loaded: RefCell::new(false),
                running: RefCell::new(false),
                calls: RefCell::new(Vec::new()),
            }
        }
    }

    impl Launchctl for Fake {
        fn run(&self, args: &[String]) -> io::Result<LaunchctlOutput> {
            self.calls.borrow_mut().push(args.join(" "));
            let ok = |stdout: &str| {
                Ok(LaunchctlOutput {
                    code: 0,
                    stdout: stdout.into(),
                })
            };
            match args[0].as_str() {
                "print" if *self.loaded.borrow() => ok(if *self.running.borrow() {
                    include_str!("../fixtures/service/launchctl-print-running.txt")
                } else {
                    include_str!("../fixtures/service/launchctl-print-waiting.txt")
                }),
                "print" => Ok(LaunchctlOutput {
                    code: 113,
                    stdout: "Could not find service".into(),
                }),
                "bootstrap" => {
                    *self.loaded.borrow_mut() = true;
                    *self.running.borrow_mut() = true; // RunAtLoad
                    ok("")
                }
                "bootout" => {
                    *self.loaded.borrow_mut() = false;
                    *self.running.borrow_mut() = false;
                    ok("")
                }
                "kickstart" => {
                    *self.running.borrow_mut() = true;
                    ok("")
                }
                other => panic!("unexpected launchctl verb {other}"),
            }
        }
    }

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("cyd-svc-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn config(dir: &Path) -> ServiceConfig {
        let daemon = dir.join("cyd-hostd");
        fs::write(&daemon, "#!/bin/sh\n").unwrap();
        fs::set_permissions(&daemon, fs::Permissions::from_mode(0o755)).unwrap();
        ServiceConfig {
            label: default_label(),
            daemon_path: daemon,
            launch_agents_dir: dir.join("LaunchAgents"),
            log_dir: dir.join("Logs"),
            uid: 501,
        }
    }

    #[test]
    fn plist_matches_the_golden_fixture() {
        let config = ServiceConfig {
            label: default_label(),
            daemon_path: "/opt/cyd/bin/cyd-hostd".into(),
            launch_agents_dir: "/unused".into(),
            log_dir: "/Users/example/Library/Logs/CYD System Monitor".into(),
            uid: 501,
        };
        assert_eq!(
            render_plist(&config),
            include_str!("../fixtures/service/hostd.plist")
        );
    }

    #[test]
    fn plist_escapes_markup_in_paths() {
        let config = ServiceConfig {
            label: "x".into(),
            daemon_path: "/a&b/<d>".into(),
            launch_agents_dir: "/".into(),
            log_dir: "/l".into(),
            uid: 1,
        };
        assert!(render_plist(&config).contains("/a&amp;b/&lt;d&gt;"));
    }

    #[test]
    fn print_output_is_parsed_from_the_top_level_only() {
        assert_eq!(
            parse_print(include_str!(
                "../fixtures/service/launchctl-print-running.txt"
            )),
            (true, Some(4242))
        );
        assert_eq!(
            parse_print(include_str!(
                "../fixtures/service/launchctl-print-waiting.txt"
            )),
            (false, None)
        );
        assert_eq!(parse_print(""), (false, None));
    }

    #[test]
    fn install_writes_the_plist_and_bootstraps_then_status_reports_running() {
        let dir = temp("install");
        let config = config(&dir);
        let fake = Fake::new();
        let report = perform(&config, &fake, Action::Install { load: true }, false).unwrap();
        assert!(config.plist_path().is_file());
        assert_eq!(
            fs::metadata(config.plist_path())
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o644
        );
        assert_eq!(
            report.status,
            Some(ServiceStatus {
                installed: true,
                loaded: true,
                running: true,
                pid: Some(4242)
            })
        );
        assert!(fake
            .calls
            .borrow()
            .iter()
            .any(|c| c == &format!("bootstrap gui/501 {}", config.plist_path().display())));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn stop_start_restart_map_to_bootout_bootstrap_kickstart() {
        let dir = temp("cycle");
        let config = config(&dir);
        let fake = Fake::new();
        perform(&config, &fake, Action::Install { load: true }, false).unwrap();
        let stopped = perform(&config, &fake, Action::Stop, false).unwrap();
        assert_eq!(
            stopped.status,
            Some(ServiceStatus {
                installed: true,
                ..Default::default()
            })
        );
        perform(&config, &fake, Action::Start, false).unwrap();
        assert!(*fake.running.borrow());
        fake.calls.borrow_mut().clear();
        perform(&config, &fake, Action::Restart, false).unwrap();
        assert!(fake
            .calls
            .borrow()
            .contains(&"kickstart -k gui/501/com.cydsystemmonitor.hostd".to_string()));
        // A loaded but stopped job is kickstarted, not re-bootstrapped.
        *fake.running.borrow_mut() = false;
        fake.calls.borrow_mut().clear();
        perform(&config, &fake, Action::Start, false).unwrap();
        assert!(fake
            .calls
            .borrow()
            .iter()
            .any(|c| c.starts_with("kickstart ")));
        assert!(!fake
            .calls
            .borrow()
            .iter()
            .any(|c| c.starts_with("bootstrap")));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn start_and_restart_refuse_when_not_installed() {
        let dir = temp("missing");
        let config = config(&dir);
        let fake = Fake::new();
        assert!(perform(&config, &fake, Action::Start, false).is_err());
        assert!(perform(&config, &fake, Action::Restart, false).is_err());
        let status = perform(&config, &fake, Action::Status, false).unwrap();
        assert_eq!(status.status, Some(ServiceStatus::default()));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn uninstall_removes_only_the_plist_and_registration() {
        let dir = temp("uninstall");
        let config = config(&dir);
        let fake = Fake::new();
        let state = dir.join("state");
        fs::create_dir_all(&state).unwrap();
        fs::write(state.join("identity"), "host=00\n").unwrap();
        perform(&config, &fake, Action::Install { load: true }, false).unwrap();
        fs::write(config.log_dir.join("hostd.log"), "x").unwrap();
        let report = perform(&config, &fake, Action::Uninstall, false).unwrap();
        assert_eq!(report.status, Some(ServiceStatus::default()));
        assert!(!config.plist_path().exists());
        assert!(state.join("identity").exists());
        assert!(config.log_dir.join("hostd.log").exists());
        assert!(config.daemon_path.exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn dry_run_prints_the_plist_and_commands_and_writes_nothing() {
        let dir = temp("dry");
        let config = config(&dir);
        let dry = DryRunLaunchctl {
            commands: RefCell::new(Vec::new()),
        };
        let report = perform(&config, &dry, Action::Install { load: true }, true).unwrap();
        assert!(!config.launch_agents_dir.exists());
        assert!(!config.log_dir.exists());
        assert!(report
            .lines
            .iter()
            .any(|l| l.contains("com.cydsystemmonitor.hostd")));
        assert_eq!(
            dry.commands.borrow().as_slice(),
            [format!(
                "launchctl bootstrap gui/501 {}",
                config.plist_path().display()
            )]
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn install_refuses_a_missing_or_non_executable_daemon() {
        let dir = temp("nodaemon");
        let mut config = config(&dir);
        config.daemon_path = dir.join("absent");
        assert!(perform(&config, &Fake::new(), Action::Install { load: true }, false).is_err());
        assert!(!config.launch_agents_dir.exists());
        let _ = fs::remove_dir_all(&dir);
    }
}
