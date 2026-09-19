// SPDX-License-Identifier: GPL-3.0-only

//! Host CLI: pairing, live CPU, and recovery commands for a foreground
//! session. The LaunchAgent lifecycle belongs to TB08.

mod identity;
mod link;

use std::process::ExitCode;
use std::thread::sleep;
use std::time::Duration;

use link::{Link, LinkError};
use protocol::{CommandOperation, CommandResult};

const USAGE: &str = "\
usage: cyd-host <command> [--port <serial device>]

commands:
  run                Pair if needed, then send live CPU once per second.
                     Reconnects (with a new session) if the link is lost.
  unpair             Ask the paired display to forget this Mac.
  reset-calibration  Ask the paired display to clear touch calibration
                     and restart into guided calibration.
  forget             Forget the stored display on this Mac only.
  help               Show this message.
";

fn port_arg(args: &[String]) -> Option<String> {
    let at = args.iter().position(|a| a == "--port")?;
    args.get(at + 1).cloned()
}

fn connect(port: &str) -> Result<Link, LinkError> {
    let mut identity = identity::load_or_create()?;
    Link::connect(port, &mut identity)
}

fn run(port: &str) -> ExitCode {
    loop {
        match connect(port).and_then(|mut link| link.serve()) {
            Err(LinkError::Io(error)) => {
                eprintln!("link lost ({error}); reconnecting in 2 s with a new session");
                sleep(Duration::from_secs(2));
            }
            Err(error) => {
                eprintln!("error: {error}");
                return ExitCode::FAILURE;
            }
            Ok(()) => return ExitCode::SUCCESS,
        }
    }
}

fn command(port: &str, operation: CommandOperation, forget_device: bool) -> ExitCode {
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
        ("forget", _) => match identity::load_or_create() {
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
        },
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
        for word in ["run", "unpair", "reset-calibration", "forget", "help"] {
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
