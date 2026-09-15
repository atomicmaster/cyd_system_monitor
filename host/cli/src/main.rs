// SPDX-License-Identifier: GPL-3.0-only

//! Host CLI skeleton. Pairing, diagnostics, and configuration commands are
//! added by their tracer tickets.

use std::process::ExitCode;

const USAGE: &str = "\
usage: cyd-host <command>

commands:
  help    Show this message.
";

fn main() -> ExitCode {
    let command = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "help".to_string());
    match command.as_str() {
        "help" | "-h" | "--help" => {
            print!("{USAGE}");
            ExitCode::SUCCESS
        }
        other => {
            eprintln!("error: unknown command '{other}'");
            eprint!("{USAGE}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn usage_mentions_help() {
        assert!(super::USAGE.contains("help"));
    }
}
