// SPDX-License-Identifier: GPL-3.0-only

//! Foreground Host session over the board's serial port: handshake, pairing,
//! one-second CPU snapshots, and the unpair / calibration-reset commands.

use std::io;
use std::thread::sleep;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use metrics::{Collector, MetricAvailability};
use platform_macos::battery::{BatteryCollector, BatteryReading};
use platform_macos::cpu::CpuCollector;
use platform_macos::ram::RamCollector;
use platform_macos::storage::{StorageCollector, StorageReading};
use protocol::{
    Availability, BatterySnapshot, CommandOperation, CommandResult, CpuSnapshot, Freshness,
    RamSnapshot, StorageSnapshot,
};
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
                client.set_local_offset_minutes(platform_macos::time::local_offset_minutes());
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
        let mut cpu_collector = CpuCollector::new();
        let mut ram_collector = RamCollector::new();
        let mut storage_collector = StorageCollector::new();
        let mut battery_collector = BatteryCollector::new();
        let epoch = Instant::now();
        let mut next = Instant::now();
        let mut buf = [0u8; 512];
        loop {
            let now_ms = epoch.elapsed().as_millis() as u64;
            let sample = cpu_collector.collect(now_ms);
            let snapshot = CpuSnapshot {
                availability: availability(sample.availability),
                utilization_per_mille: sample.value.unwrap_or(0),
                source_age_ms: 0,
                freshness: Freshness::Fresh,
                configuration_revision: 0,
            };
            let ram_sample = ram_collector.collect(now_ms);
            let ram = RamSnapshot {
                availability: availability(ram_sample.availability),
                used_bytes: ram_sample.value.map_or(0, |r| r.used_bytes),
                total_bytes: ram_sample.value.map_or(0, |r| r.total_bytes),
                source_age_ms: 0,
                freshness: Freshness::Fresh,
            };
            let storage = storage_snapshot(storage_collector.collect(now_ms).value);
            let battery = battery_snapshot(battery_collector.collect(now_ms).value);
            self.client
                .set_local_offset_minutes(platform_macos::time::local_offset_minutes());
            let frame = self
                .client
                .snapshot(
                    &snapshot,
                    Some(&ram),
                    storage.as_ref(),
                    battery.as_ref(),
                    Some(utc_ms()),
                )
                .ok_or(LinkError::Timeout("sending: the link is no longer paired"))?;
            self.port.write_all(&frame)?;
            if let Some(g) = &storage {
                println!(
                    "storage {:.1} / {:.1} GB  read {} B/s  write {} B/s",
                    (g.total_bytes - g.available_bytes) as f64 / 1e9,
                    g.total_bytes as f64 / 1e9,
                    g.read_bytes_per_s,
                    g.write_bytes_per_s
                );
            }
            if let Some(g) = &battery {
                println!(
                    "battery charge {:?} {:.1}% state {} ({} min)  health {:?} {:.1}% condition {} cycles {}",
                    g.charge_availability,
                    f64::from(g.charge_per_mille) / 10.0,
                    g.state,
                    g.time_remaining_min,
                    g.condition_availability,
                    f64::from(g.max_capacity_per_mille) / 10.0,
                    g.condition,
                    g.cycle_count
                );
            }
            match (sample.value, ram_sample.value) {
                (Some(v), Some(r)) => println!(
                    "cpu {:.1}%  ram {:.1} / {:.1} GB",
                    f64::from(v) / 10.0,
                    r.used_bytes as f64 / (1u64 << 30) as f64,
                    r.total_bytes as f64 / (1u64 << 30) as f64
                ),
                (Some(v), None) => println!("cpu {:.1}%  ram unavailable", f64::from(v) / 10.0),
                (None, _) => println!("cpu unavailable"),
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

fn availability(a: MetricAvailability) -> Availability {
    match a {
        MetricAvailability::Available => Availability::Available,
        MetricAvailability::Unsupported => Availability::Unsupported,
        MetricAvailability::Absent => Availability::Absent,
        MetricAvailability::TemporarilyUnavailable => Availability::TemporarilyUnavailable,
        MetricAvailability::Error => Availability::Error,
    }
}

/// The wire group for one second of storage collection. A half that is not
/// available carries zero values and its availability; capacity and I/O are
/// independent.
fn storage_snapshot(reading: Option<StorageReading>) -> Option<StorageSnapshot> {
    let reading = reading?;
    let capacity = reading
        .capacity
        .value
        .filter(|c| c.available_bytes <= c.total_bytes);
    let io = reading.io.value;
    Some(StorageSnapshot {
        capacity_availability: match capacity {
            Some(_) => Availability::Available,
            None => match availability(reading.capacity.availability) {
                Availability::Available => Availability::Error, // no value behind it
                other => other,
            },
        },
        total_bytes: capacity.map_or(0, |c| c.total_bytes),
        available_bytes: capacity.map_or(0, |c| c.available_bytes),
        capacity_age_ms: if capacity.is_some() {
            reading.capacity_age_ms
        } else {
            0
        },
        io_availability: availability(reading.io.availability),
        read_bytes_per_s: io.map_or(0, |r| r.read_bytes_per_s),
        write_bytes_per_s: io.map_or(0, |r| r.write_bytes_per_s),
        io_age_ms: 0,
    })
}

/// The wire group for one second of battery collection. A half that is not
/// available carries zero values (times and cycle counts the unknown sentinel)
/// and its availability, so a batteryless host is `Absent`, never a 0% charge.
/// A condition source that lacks one field reports that field as 0 (capacity)
/// or the sentinel (cycles): a working battery has no 0% health capacity.
fn battery_snapshot(reading: Option<BatteryReading>) -> Option<BatterySnapshot> {
    let reading = reading?;
    let no_value = |a: MetricAvailability| match availability(a) {
        Availability::Available => Availability::Error, // no value behind it
        other => other,
    };
    let charge = reading.charge.value;
    let condition = reading.condition.value;
    Some(BatterySnapshot {
        charge_availability: if charge.is_some() {
            Availability::Available
        } else {
            no_value(reading.charge.availability)
        },
        charge_per_mille: charge.map_or(0, |c| c.per_mille.min(1000)),
        state: charge.map_or(0, |c| c.state as u8),
        time_remaining_min: charge
            .and_then(|c| c.time_remaining_min)
            .unwrap_or(protocol::BATTERY_UNKNOWN_U16),
        charge_age_ms: if charge.is_some() {
            reading.charge_age_ms
        } else {
            0
        },
        condition_availability: if condition.is_some() {
            Availability::Available
        } else {
            no_value(reading.condition.availability)
        },
        max_capacity_per_mille: condition
            .and_then(|c| c.max_capacity_per_mille)
            .unwrap_or(0),
        condition: condition.map_or(0, |c| c.condition as u8),
        cycle_count: condition
            .and_then(|c| c.cycle_count)
            .unwrap_or(protocol::BATTERY_UNKNOWN_U16),
        condition_age_ms: if condition.is_some() {
            reading.condition_age_ms
        } else {
            0
        },
    })
}

fn protocol_error(error: protocol::Error) -> LinkError {
    LinkError::Io(io::Error::new(
        io::ErrorKind::InvalidData,
        format!("encode failed: {error:?}"),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use metrics::battery::{
        BatteryChargeReading, BatteryCondition, BatteryConditionReading, BatteryState,
    };
    use metrics::MetricSample;

    fn reading(
        charge: MetricSample<BatteryChargeReading>,
        condition: MetricSample<BatteryConditionReading>,
    ) -> BatteryReading {
        BatteryReading {
            charge,
            charge_age_ms: 12_000,
            condition,
            condition_age_ms: 600_000,
        }
    }

    #[test]
    fn a_batteryless_host_is_absent_on_the_wire_never_zero_percent() {
        let g = battery_snapshot(Some(reading(
            MetricSample::unavailable(MetricAvailability::Absent),
            MetricSample::unavailable(MetricAvailability::Absent),
        )))
        .unwrap();
        assert_eq!(g.charge_availability, Availability::Absent);
        assert_eq!(g.condition_availability, Availability::Absent);
        assert_eq!((g.charge_per_mille, g.charge_age_ms), (0, 0));
        assert_eq!(g.time_remaining_min, protocol::BATTERY_UNKNOWN_U16);
        assert_eq!(g.cycle_count, protocol::BATTERY_UNKNOWN_U16);
    }

    #[test]
    fn live_halves_carry_values_and_their_own_ages_and_partial_health_is_zero_or_unknown() {
        let g = battery_snapshot(Some(reading(
            MetricSample::available(BatteryChargeReading {
                per_mille: 870,
                state: BatteryState::Charging,
                time_remaining_min: None,
            }),
            MetricSample::available(BatteryConditionReading {
                max_capacity_per_mille: None,
                condition: BatteryCondition::ServiceRecommended,
                cycle_count: None,
            }),
        )))
        .unwrap();
        assert_eq!(
            (g.charge_availability, g.charge_per_mille, g.state),
            (Availability::Available, 870, 1)
        );
        assert_eq!(
            (g.time_remaining_min, g.charge_age_ms),
            (protocol::BATTERY_UNKNOWN_U16, 12_000)
        );
        assert_eq!(
            (
                g.condition_availability,
                g.max_capacity_per_mille,
                g.condition
            ),
            (Availability::Available, 0, 2)
        );
        assert_eq!(
            (g.cycle_count, g.condition_age_ms),
            (protocol::BATTERY_UNKNOWN_U16, 600_000)
        );
    }

    #[test]
    fn a_missing_condition_leaves_the_charge_live_and_errors_carry_no_values() {
        let g = battery_snapshot(Some(reading(
            MetricSample::unavailable(MetricAvailability::Error),
            MetricSample::unavailable(MetricAvailability::TemporarilyUnavailable),
        )))
        .unwrap();
        assert_eq!(g.charge_availability, Availability::Error);
        assert_eq!(
            g.condition_availability,
            Availability::TemporarilyUnavailable
        );
        assert_eq!((g.charge_age_ms, g.condition_age_ms, g.state), (0, 0, 0));
    }
}
