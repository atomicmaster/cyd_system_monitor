// SPDX-License-Identifier: GPL-3.0-only

//! Host CLI: pairing, live snapshots in the foreground, recovery commands, the
//! LaunchAgent lifecycle and the client of the daemon's control socket. The
//! daemon (`cyd-hostd`) owns the serial port; while it runs, commands that
//! need the display go through it and never open the port themselves.

mod service;

use std::io;
use std::process::ExitCode;
use std::rc::Rc;
use std::thread::sleep;
use std::time::Duration;

use hostcore::control::{self, json_string, simple_request};
use hostcore::daemon::Clock;
use hostcore::identity;
use hostcore::link::{Collected, Collectors, Link, LinkError, ServeEnd, ServeObserver};
use hostcore::lock::{InstanceLock, LockError};
use hostcore::paths;
use metrics::wifi_context::{FixtureProvider, WifiContextProvider};
use platform_macos::wifi_context_server::{current_uid, ServerConfig, WifiContextServer};
use protocol::{CommandOperation, CommandResult};
use service::{Action, DryRunLaunchctl, Launchctl, ServiceConfig, SystemLaunchctl};

const USAGE: &str = "\
usage: cyd-host <command> [--port <serial device>]

commands:
  run                Pair if needed, then send live snapshots once per second in
                     the foreground. Reconnects (with a new session) if the link
                     is lost. Refuses while the daemon owns the serial port.
  unpair             Ask the paired display to forget this Mac.
  reset-calibration  Ask the paired display to clear touch calibration
                     and restart into guided calibration.
  forget             Forget the stored display on this Mac only.
  status             Print the running daemon's status (one JSON line).
  monitors           List attached candidate displays (daemon required).
  pair [--port P]    Ask the daemon to start pairing; confirm the shown code on
                     the display.
  pair-cancel        Ask the daemon to abandon a pairing attempt.
  stop               Ask the daemon to stop cleanly (exit 0; not restarted).
  service <action>   Manage the per-user LaunchAgent. Actions: install,
                     uninstall, start, stop, restart, status. Options:
                       --dry-run               print the plist and commands only
                       --no-load               (install) write the plist only
                       --daemon-path <path>    installed cyd-hostd (default:
                                               next to this program)
                       --launch-agents-dir <d> (default ~/Library/LaunchAgents)
                       --log-dir <d>           (default ~/Library/Logs/CYD System
                                               Monitor)
  help               Show this message.

When the daemon runs, unpair, reset-calibration and forget are sent to it.
`service status` exits 0 when the daemon is running and 3 otherwise.
";

fn port_arg(args: &[String]) -> Option<String> {
    option(args, "--port")
}

fn option(args: &[String], flag: &str) -> Option<String> {
    let at = args.iter().position(|a| a == flag)?;
    args.get(at + 1).cloned()
}

fn has(args: &[String], flag: &str) -> bool {
    args.iter().any(|a| a == flag)
}

/// Prints drop counters only when something was dropped.
struct PrintObserver {
    server: Option<WifiContextServer>,
}

impl ServeObserver for PrintObserver {
    fn on_snapshot(&mut self, collected: &Collected) {
        let drops = self.server.as_ref().and_then(|s| {
            let n = s.stats();
            (n.malformed + n.oversized + n.out_of_order + n.wrong_uid + n.rate_limited > 0).then(
                || {
                    format!(
                        " (dropped: {} malformed, {} oversized, {} out of order, {} wrong uid, {} rate limited)",
                        n.malformed, n.oversized, n.out_of_order, n.wrong_uid, n.rate_limited
                    )
                },
            )
        });
        for line in collected.log_lines(drops) {
            println!("{line}");
        }
    }
}

/// Takes the single-instance lock, or explains who has it.
fn take_lock() -> Result<InstanceLock, ExitCode> {
    InstanceLock::acquire(&paths::run_dir()).map_err(|error| {
        match error {
            LockError::Held { .. } => eprintln!("error: {error}"),
            LockError::Io(_) => eprintln!("error: {error}"),
        }
        ExitCode::FAILURE
    })
}

fn connect(port: &str) -> Result<Link, LinkError> {
    let mut identity = identity::load_or_create()?;
    Link::connect(port, &mut identity)
}

fn run(port: &str) -> ExitCode {
    let _lock = match take_lock() {
        Ok(lock) => lock,
        Err(code) => return code,
    };
    let clock = Clock::new();
    // The WiFi context provider socket. Failing to open it costs only the
    // optional context: traffic and Host Station Identity still flow.
    let (server, provider): (Option<WifiContextServer>, Box<dyn WifiContextProvider>) =
        match WifiContextServer::start(ServerConfig {
            dir: paths::run_dir(),
            expected_uid: current_uid(),
            now_ms: clock.now_fn(),
        }) {
            Ok(server) => {
                println!("WiFi context socket: {}", server.socket_path().display());
                let provider = Box::new(server.provider());
                (Some(server), provider)
            }
            Err(error) => {
                println!("WiFi context provider unavailable ({error}); continuing without it");
                (None, Box::new(FixtureProvider::absent()))
            }
        };
    let mut collectors = Collectors::new(provider);
    let mut observer = PrintObserver { server };
    let (now, wall) = (clock.clone(), clock);
    loop {
        let outcome = connect(port).and_then(|mut link| {
            link.serve(
                &mut collectors,
                &|| now.now_ms(),
                &|| wall.wall_ms(),
                &mut observer,
            )
        });
        match outcome {
            Err(LinkError::Io(error)) => {
                eprintln!("link lost ({error}); reconnecting in 2 s with a new session");
                sleep(Duration::from_secs(2));
            }
            Ok(ServeEnd::Resync) => println!("resynchronizing after sleep/wake with a new session"),
            Err(error) => {
                eprintln!("error: {error}");
                return ExitCode::FAILURE;
            }
            Ok(_) => return ExitCode::SUCCESS,
        }
    }
}

/// Some(response line) when a daemon answers on its control socket, None when
/// there is no daemon to talk to.
fn ask_daemon(line: &str) -> Option<io::Result<String>> {
    match control::request(&paths::run_dir(), line, Duration::from_secs(25)) {
        Err(e)
            if matches!(
                e.kind(),
                io::ErrorKind::NotFound | io::ErrorKind::ConnectionRefused
            ) =>
        {
            None
        }
        other => Some(other),
    }
}

/// Prints the daemon's reply; success means `"ok":true`.
fn show_reply(reply: io::Result<String>) -> ExitCode {
    match reply {
        Ok(text) => {
            println!("{text}");
            if text.contains("\"ok\":true") {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            }
        }
        Err(e) => {
            eprintln!("error: the daemon did not answer: {e}");
            ExitCode::FAILURE
        }
    }
}

fn daemon_command(cmd: &str, port: Option<&str>) -> ExitCode {
    let line = match port {
        Some(port) => format!(
            "{{\"v\":{},\"cmd\":{},\"port\":{}}}",
            control::IPC_VERSION,
            json_string(cmd),
            json_string(port)
        ),
        None => simple_request(cmd),
    };
    match ask_daemon(&line) {
        Some(reply) => show_reply(reply),
        None => {
            eprintln!("error: the daemon is not running (no control socket); start it with `cyd-host service start`");
            ExitCode::FAILURE
        }
    }
}

fn command(port: &str, operation: CommandOperation, forget_device: bool) -> ExitCode {
    let name = match operation {
        CommandOperation::Unpair => "unpair",
        CommandOperation::CalibrationReset => "reset_calibration",
    };
    // The daemon owns the port while it runs: never open it from here then.
    if let Some(reply) = ask_daemon(&simple_request(name)) {
        return show_reply(reply);
    }
    let _lock = match take_lock() {
        Ok(lock) => lock,
        Err(code) => return code,
    };
    let outcome = connect(port).and_then(|mut link| link.command(operation));
    match outcome {
        Ok(CommandResult::Applied | CommandResult::AlreadyApplied) => {
            if forget_device {
                if let Ok(mut id) = identity::load_or_create() {
                    id.device = None;
                    let _ = identity::save(&id);
                }
            }
            println!("done");
            ExitCode::SUCCESS
        }
        Ok(CommandResult::Rejected) => {
            eprintln!("error: the display rejected the command");
            ExitCode::FAILURE
        }
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}

fn forget() -> ExitCode {
    if let Some(reply) = ask_daemon(&simple_request("forget")) {
        return show_reply(reply);
    }
    match identity::load_or_create() {
        Ok(mut id) => {
            id.device = None;
            match identity::save(&id) {
                Ok(()) => {
                    println!("forgot the stored display");
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("error: {e}");
                    ExitCode::FAILURE
                }
            }
        }
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}

fn service_command(args: &[String]) -> ExitCode {
    let action = match args.get(1).map(String::as_str) {
        Some("install") => Action::Install {
            load: !has(args, "--no-load"),
        },
        Some("uninstall") => Action::Uninstall,
        Some("start") => Action::Start,
        Some("stop") => Action::Stop,
        Some("restart") => Action::Restart,
        Some("status") => Action::Status,
        _ => {
            eprintln!(
                "error: service needs one of install, uninstall, start, stop, restart, status"
            );
            eprint!("{USAGE}");
            return ExitCode::FAILURE;
        }
    };
    let dry_run = has(args, "--dry-run");
    let config = ServiceConfig {
        label: service::default_label(),
        daemon_path: option(args, "--daemon-path")
            .map(Into::into)
            .unwrap_or_else(service::default_daemon_path),
        launch_agents_dir: option(args, "--launch-agents-dir")
            .map(Into::into)
            .unwrap_or_else(paths::launch_agents_dir),
        log_dir: option(args, "--log-dir")
            .map(Into::into)
            .unwrap_or_else(paths::log_dir),
        uid: current_uid(),
    };
    if !config.daemon_path.is_absolute() {
        eprintln!("error: --daemon-path must be an absolute path");
        return ExitCode::FAILURE;
    }
    let dry = Rc::new(DryRunLaunchctl {
        commands: Default::default(),
    });
    let ctl: &dyn Launchctl = if dry_run {
        dry.as_ref()
    } else {
        &SystemLaunchctl
    };
    match service::perform(&config, ctl, action, dry_run) {
        Ok(report) => {
            for line in &report.lines {
                println!("{line}");
            }
            if dry_run {
                println!("dry run: nothing was written and no launchctl command was run");
            }
            match (action, report.status) {
                (Action::Status, Some(status)) => {
                    println!("{}", status.describe());
                    if status.running {
                        ExitCode::SUCCESS
                    } else {
                        ExitCode::from(3)
                    }
                }
                (_, Some(status)) => {
                    println!("{}", status.describe());
                    ExitCode::SUCCESS
                }
                _ => ExitCode::SUCCESS,
            }
        }
        Err(message) => {
            eprintln!("error: {message}");
            ExitCode::FAILURE
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let name = args.first().map(String::as_str).unwrap_or("help");
    let needs_port = matches!(name, "run" | "unpair" | "reset-calibration");
    let port = port_arg(&args);
    if needs_port && port.is_none() {
        eprintln!("error: '{name}' needs --port <serial device>");
        eprint!("{USAGE}");
        return ExitCode::FAILURE;
    }
    match (name, port) {
        ("help" | "-h" | "--help", _) => {
            print!("{USAGE}");
            ExitCode::SUCCESS
        }
        ("run", Some(port)) => run(&port),
        ("unpair", Some(port)) => command(&port, CommandOperation::Unpair, true),
        ("reset-calibration", Some(port)) => {
            command(&port, CommandOperation::CalibrationReset, false)
        }
        ("forget", _) => forget(),
        ("status", _) => daemon_command("status", None),
        ("monitors", _) => daemon_command("monitors", None),
        ("pair", port) => daemon_command("pair_start", port.as_deref()),
        ("pair-cancel", _) => daemon_command("pair_cancel", None),
        ("stop", _) => daemon_command("stop", None),
        ("service", _) => service_command(&args),
        (other, _) => {
            eprintln!("error: unknown command '{other}'");
            eprint!("{USAGE}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn usage_mentions_every_command() {
        for word in [
            "run",
            "unpair",
            "reset-calibration",
            "forget",
            "status",
            "monitors",
            "pair-cancel",
            "stop",
            "service",
            "help",
        ] {
            assert!(super::USAGE.contains(word), "{word}");
        }
    }

    #[test]
    fn port_argument_is_parsed() {
        let args: Vec<String> = ["run", "--port", "/dev/cu.x"].map(String::from).into();
        assert_eq!(super::port_arg(&args).as_deref(), Some("/dev/cu.x"));
        assert_eq!(super::port_arg(&args[..2]), None);
    }
}
