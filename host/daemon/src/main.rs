// SPDX-License-Identifier: GPL-3.0-only

//! `cyd-hostd`: the per-user Host daemon. Owns the USB serial port, the
//! WiFi context provider socket and the control socket. Exit status 0 means a
//! requested, clean stop (SIGTERM, SIGINT or the `stop` command), which a
//! LaunchAgent with `KeepAlive/SuccessfulExit=false` does not restart; any
//! failure exits non-zero.

use std::ffi::c_int;
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::sleep;
use std::time::Duration;

use hostcore::daemon::{Daemon, DaemonConfig};
use hostcore::logging::{Logger, RotatingLog};

const USAGE: &str = "\
usage: cyd-hostd [--port <serial device>] [--log-dir <directory>]

Runs the per-user Host daemon in the foreground. Normally started by its
LaunchAgent (`cyd-host service install`). Without --log-dir it logs to stderr.
Environment: CYD_RUN_DIR (sockets and lock), CYD_HOME (pairing identity).
";

static SIGNALLED: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn signal(signum: c_int, handler: extern "C" fn(c_int)) -> usize;
}

extern "C" fn on_signal(_: c_int) {
    SIGNALLED.store(true, Ordering::Relaxed);
}

fn value(args: &[String], flag: &str) -> Option<String> {
    let at = args.iter().position(|a| a == flag)?;
    args.get(at + 1).cloned()
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "-h" || a == "--help") {
        print!("{USAGE}");
        return ExitCode::SUCCESS;
    }
    if let Some(unknown) = args
        .iter()
        .find(|a| a.starts_with("--") && !["--port", "--log-dir"].contains(&a.as_str()))
    {
        eprintln!("error: unknown option '{unknown}'\n{USAGE}");
        return ExitCode::FAILURE;
    }
    let logger = match value(&args, "--log-dir") {
        Some(dir) => match RotatingLog::open(std::path::Path::new(&dir)) {
            Ok(log) => Logger::File(std::sync::Mutex::new(log)),
            Err(e) => {
                eprintln!("error: cannot open the log in {dir}: {e}");
                return ExitCode::FAILURE;
            }
        },
        None => Logger::Stderr,
    };
    // SAFETY: the handler only stores to an atomic, which is async-signal-safe.
    unsafe {
        signal(15, on_signal); // SIGTERM
        signal(2, on_signal); // SIGINT
    }
    let config = DaemonConfig::system(Arc::new(logger), value(&args, "--port"));
    let daemon = match Daemon::start(config) {
        Ok(daemon) => daemon,
        Err(error) => {
            eprintln!("cyd-hostd: {error}");
            return ExitCode::FAILURE;
        }
    };
    let stop = daemon.stop_flag();
    loop {
        if SIGNALLED.load(Ordering::Relaxed) {
            stop.store(true, Ordering::Relaxed);
        }
        if !daemon.is_running() {
            break;
        }
        sleep(Duration::from_millis(100));
    }
    let clean = stop.load(Ordering::Relaxed);
    daemon.shutdown();
    if clean {
        ExitCode::SUCCESS
    } else {
        eprintln!("cyd-hostd: the supervisor stopped unexpectedly");
        ExitCode::FAILURE
    }
}
