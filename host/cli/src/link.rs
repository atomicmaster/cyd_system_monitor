// SPDX-License-Identifier: GPL-3.0-only

//! Foreground Host session over the board's serial port: handshake, pairing,
//! one-second CPU snapshots, and the unpair / calibration-reset commands.

use std::io;
use std::thread::sleep;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use metrics::{CpuSampler, MetricAvailability};
use protocol::{Availability, CommandOperation, CommandResult, CpuSnapshot, Freshness};
use transport::client::{Event, HostClient, Phase, Refusal};
use transport::serial::SerialPort;

use crate::identity::{self, Identity};

/// Opening the port resets the ESP32; give it time to boot to the console.
const BOOT_WAIT: Duration = Duration::from_millis(3_000);
const HELLO_RETRY: Duration = Duration::from_millis(1_500);
/// Longer than the board's own touch-confirmation timeout.
const HANDSHAKE_DEADLINE: Duration = Duration::from_secs(75);
const SNAPSHOT_PERIOD: Duration = Duration::from_secs(1);

#[derive(Debug)]
pub enum LinkError {
    Io(io::Error),
    Refused(Refusal),
    Timeout(&'static str),
}

impl From<io::Error> for LinkError {
    fn from(error: io::Error) -> Self {
        LinkError::Io(error)
    }
}

impl std::fmt::Display for LinkError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LinkError::Io(e) => write!(f, "serial error: {e}"),
            LinkError::Refused(Refusal::Incompatible) => {
                write!(f, "the display speaks an incompatible protocol version")
            }
            LinkError::Refused(Refusal::NotPairedWithYou) => write!(
                f,
                "the display is paired with another Mac; unpair it from that Mac, or send \
                 DEV:UNPAIR on its console"
            ),
            LinkError::Refused(Refusal::Declined) => {
                write!(f, "pairing was declined or timed out on the display")
            }
            LinkError::Refused(other) => write!(f, "the display refused the request ({other:?})"),
            LinkError::Timeout(what) => write!(f, "timed out {what}"),
        }
    }
}

fn utc_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn matching_code() -> io::Result<u32> {
    let bytes: [u8; 4] = identity::random_bytes()?;
    Ok(100_000 + u32::from_le_bytes(bytes) % 900_000)
}

/// One serial connection with a completed handshake.
pub struct Link {
    port: SerialPort,
    client: HostClient,
}

impl Link {
    /// Opens the port, negotiates, and pairs (asking the operator to confirm
    /// on the display the first time). Persists the device identity.
    pub fn connect(port_path: &str, identity: &mut Identity) -> Result<Link, LinkError> {
        let mut port = SerialPort::open(port_path)?;
        println!("Opened {port_path}; waiting for the board to boot...");
        sleep(BOOT_WAIT);

        let code = matching_code()?;
        let session = identity::random_bytes()?;
        let mut client = HostClient::new(identity.host, identity.device, code, session);
        let started = Instant::now();
        let mut last_hello: Option<Instant> = None;
        let mut announced = false;
        let mut buf = [0u8; 512];

        while started.elapsed() < HANDSHAKE_DEADLINE {
            if client.phase() != Phase::PairSent
                && last_hello.is_none_or(|t| t.elapsed() >= HELLO_RETRY)
            {
                // Idempotent: harmless if the board is already in link mode.
                port.write_all(b"CYD:LINK\n")?;
                sleep(Duration::from_millis(150));
                port.write_all(&client.start(Some(utc_ms())).map_err(protocol_error)?)?;
                last_hello = Some(Instant::now());
            }
            let n = port.read_timeout(&mut buf, Duration::from_millis(100))?;
            let out = client.receive(&buf[..n]);
            for frame in &out.frames {
                port.write_all(frame)?;
            }
            for event in out.events {
                match event {
                    Event::Negotiated {
                        device_major,
                        device_minor,
                    } => {
                        println!("Display speaks protocol {device_major}.{device_minor}.");
                    }
                    Event::Refused(reason) => return Err(LinkError::Refused(reason)),
                    Event::Paired { device_identity } => {
                        if identity.device != Some(device_identity) {
                            identity.device = Some(device_identity);
                            identity::save(identity)?;
                        }
                        println!("Paired. Sending live data.");
                        return Ok(Link { port, client });
                    }
                    Event::CommandReply { .. } => {}
                }
            }
            if client.phase() == Phase::PairSent && !announced && started.elapsed() > Duration::ZERO
            {
                announced = true;
                println!("Waiting for the display. If it asks, confirm code {code:06}.");
            }
        }
        Err(LinkError::Timeout("waiting for the display to pair"))
    }

    /// Sends one snapshot per second forever; returns only on a link error.
    pub fn serve(&mut self) -> Result<(), LinkError> {
        let mut sampler = CpuSampler::default();
        let epoch = Instant::now();
        let mut next = Instant::now();
        let mut buf = [0u8; 512];
        loop {
            let now_ms = epoch.elapsed().as_millis() as u64;
            let sample = match platform_macos::cpu::read_ticks() {
                Some(ticks) => sampler.sample(ticks, now_ms),
                None => metrics::MetricSample::unavailable(MetricAvailability::Error),
            };
            let snapshot = CpuSnapshot {
                availability: match sample.availability {
                    MetricAvailability::Available => Availability::Available,
                    MetricAvailability::Unsupported => Availability::Unsupported,
                    MetricAvailability::Absent => Availability::Absent,
                    MetricAvailability::TemporarilyUnavailable => {
                        Availability::TemporarilyUnavailable
                    }
                    MetricAvailability::Error => Availability::Error,
                },
                utilization_per_mille: sample.value.unwrap_or(0),
                source_age_ms: 0,
                freshness: Freshness::Fresh,
                configuration_revision: 0,
            };
            let frame = self
                .client
                .snapshot(&snapshot, Some(utc_ms()))
                .ok_or(LinkError::Timeout("sending: the link is no longer paired"))?;
            self.port.write_all(&frame)?;
            match sample.value {
                Some(v) => println!("cpu {:.1}%", f64::from(v) / 10.0),
                None => println!("cpu unavailable"),
            }
            next += SNAPSHOT_PERIOD;
            while Instant::now() < next {
                let wait = next.saturating_duration_since(Instant::now());
                let n = self
                    .port
                    .read_timeout(&mut buf, wait.min(Duration::from_millis(200)))?;
                for event in self.client.receive(&buf[..n]).events {
                    if let Event::Refused(reason) = event {
                        return Err(LinkError::Refused(reason));
                    }
                }
            }
        }
    }

    /// Sends a command and waits for its reply, retrying with the same
    /// identity so a lost frame cannot apply it twice.
    pub fn command(&mut self, operation: CommandOperation) -> Result<CommandResult, LinkError> {
        let (identity, frame) = self
            .client
            .command(operation)
            .ok_or(LinkError::Timeout("sending: the link is not paired"))?;
        self.port.write_all(&frame)?;
        let mut buf = [0u8; 512];
        for _attempt in 0..5 {
            let deadline = Instant::now() + Duration::from_secs(1);
            while Instant::now() < deadline {
                let n = self
                    .port
                    .read_timeout(&mut buf, Duration::from_millis(100))?;
                for event in self.client.receive(&buf[..n]).events {
                    if let Event::CommandReply {
                        command_identity,
                        result,
                        ..
                    } = event
                    {
                        if command_identity == identity {
                            return Ok(result);
                        }
                    }
                }
            }
            if let Some(retry) = self.client.retry_command(identity, operation) {
                self.port.write_all(&retry)?;
            }
        }
        Err(LinkError::Timeout("waiting for the command reply"))
    }
}

fn protocol_error(error: protocol::Error) -> LinkError {
    LinkError::Io(io::Error::new(
        io::ErrorKind::InvalidData,
        format!("encode failed: {error:?}"),
    ))
}
