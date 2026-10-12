// SPDX-License-Identifier: GPL-3.0-only

//! Drives the Rust `HostClient` against the real C++ firmware `HostLink`
//! (built by `./dev check firmware` as `link_cli`), so the two ends of the
//! handshake cannot drift apart unnoticed.

use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

use protocol::{
    encode_control, AgentsSnapshot, AssociationContext, Availability, BatterySnapshot,
    CommandOperation, CommandResult, ControlBody, CpuSnapshot, Envelope, Freshness, HelloBody,
    MessageType, NetworkSnapshot, RamSnapshot, RouteKind, Snapshot, StorageSnapshot, WifiCoverage,
};
use transport::client::{Event, HostClient, Phase, Refusal};

const HOST: [u8; 16] = [0xA1; 16];
const DEVICE: [u8; 16] = [0xD0; 16];
const SESSION_A: [u8; 16] = [0x11; 16];
const SESSION_B: [u8; 16] = [0x22; 16];
const CODE: u32 = 123_456;

// Device phase numbers, from firmware::domain::LinkPhase.
const AWAITING: u32 = 2;
const OPEN: u32 = 3;
const INCOMPATIBLE: u32 = 4;
const NEGOTIATED: u32 = 1;

#[derive(Debug)]
struct State {
    phase: u32,
    paired: bool,
    seen: bool,
    cpu: u32,
    ram: Option<(u64, u64)>,
    storage: Option<[u64; 4]>,
    battery: Option<[u64; 4]>,
    agents: Option<[u64; 4]>,
    network: Option<[u64; 8]>,
    code: Option<u32>,
}

struct Device {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

fn link_cli() -> PathBuf {
    if let Ok(path) = std::env::var("CYD_LINK_CLI") {
        return path.into();
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../firmware/domain/tests/build/link_cli")
}

impl Device {
    fn spawn() -> Device {
        let path = link_cli();
        assert!(
            path.exists(),
            "{} is missing: run `./dev check firmware` (or set CYD_LINK_CLI) first",
            path.display()
        );
        let mut child = Command::new(path)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .expect("spawn link_cli");
        let stdin = child.stdin.take().unwrap();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        Device {
            child,
            stdin,
            stdout,
        }
    }

    /// Sends one command; returns the frames the device wrote and its state.
    fn command(&mut self, line: &str) -> (Vec<Vec<u8>>, State) {
        writeln!(self.stdin, "{line}").unwrap();
        let mut frames = Vec::new();
        loop {
            let mut reply = String::new();
            assert!(
                self.stdout.read_line(&mut reply).unwrap() > 0,
                "link_cli exited"
            );
            let reply = reply.trim();
            if let Some(hex) = reply.strip_prefix("OUT ") {
                frames.push(from_hex(hex));
            } else if let Some(rest) = reply.strip_prefix("STATE ") {
                return (frames, parse_state(rest));
            } else {
                panic!("unexpected link_cli line: {reply}");
            }
        }
    }

    fn push(&mut self, bytes: &[u8]) -> (Vec<Vec<u8>>, State) {
        self.command(&format!("PUSH {}", to_hex(bytes)))
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        let _ = self.child.kill();
    }
}

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn from_hex(hex: &str) -> Vec<u8> {
    (0..hex.len() / 2)
        .map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

fn parse_state(rest: &str) -> State {
    let field = |name: &str| {
        rest.split_whitespace()
            .find_map(|kv| kv.strip_prefix(&format!("{name}=")))
            .unwrap_or_else(|| panic!("missing {name} in {rest}"))
            .to_string()
    };
    State {
        phase: field("phase").parse().unwrap(),
        paired: field("paired") == "1",
        seen: field("seen") == "1",
        cpu: field("cpu").parse().unwrap(),
        ram: field("ram")
            .split_once('/')
            .map(|(u, t)| (u.parse().unwrap(), t.parse().unwrap())),
        storage: field("storage").split_once('/').map(|(total, rest)| {
            let mut n = std::iter::once(total)
                .chain(rest.split('/'))
                .map(|v| v.parse().unwrap());
            std::array::from_fn(|_| n.next().unwrap())
        }),
        battery: field("battery").split_once('/').map(|(first, rest)| {
            let mut n = std::iter::once(first)
                .chain(rest.split('/'))
                .map(|v| v.parse().unwrap());
            std::array::from_fn(|_| n.next().unwrap())
        }),
        agents: field("agents").split_once('/').map(|(first, rest)| {
            let mut n = std::iter::once(first)
                .chain(rest.split('/'))
                .map(|v| v.parse().unwrap());
            std::array::from_fn(|_| n.next().unwrap())
        }),
        network: field("network").split_once('/').map(|(first, rest)| {
            let mut n = std::iter::once(first)
                .chain(rest.split('/'))
                .map(|v| v.parse().unwrap());
            std::array::from_fn(|_| n.next().unwrap())
        }),
        code: field("code").parse().ok(),
    }
}

/// Feeds device frames to the client (behind a line of log text, as on the
/// real UART) and returns the events and any frames the client wants sent.
fn deliver(client: &mut HostClient, frames: &[Vec<u8>]) -> (Vec<Event>, Vec<Vec<u8>>) {
    let mut events = Vec::new();
    let mut replies = Vec::new();
    for frame in frames {
        let mut bytes = b"I (1234) fw.dev_console: log line\r\n".to_vec();
        bytes.extend(frame);
        let out = client.receive(&bytes);
        events.extend(out.events);
        replies.extend(out.frames);
    }
    (events, replies)
}

fn snapshot(per_mille: u16) -> CpuSnapshot {
    CpuSnapshot {
        availability: Availability::Available,
        utilization_per_mille: per_mille,
        source_age_ms: 0,
        freshness: Freshness::Fresh,
        configuration_revision: 0,
    }
}

/// Runs Hello + Pair up to the point the device has answered the Pair (or is
/// waiting on touch). Returns the client's events and the device state.
fn handshake(device: &mut Device, client: &mut HostClient) -> (Vec<Event>, State) {
    let hello = client.start(Some(1_700_000_000_000)).unwrap();
    let (frames, _) = device.push(&hello);
    let (mut events, replies) = deliver(client, &frames);
    assert_eq!(
        replies.len(),
        1,
        "client should answer HelloReply with Pair"
    );
    let (frames, state) = device.push(&replies[0]);
    let (more, _) = deliver(client, &frames);
    events.extend(more);
    (events, state)
}

#[test]
fn first_pairing_needs_touch_then_live_data_flows() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    let (events, state) = handshake(&mut device, &mut client);
    assert_eq!(state.phase, AWAITING);
    assert_eq!(state.code, Some(CODE));
    assert!(!state.paired);
    assert!(matches!(
        events[0],
        Event::Negotiated {
            device_major: 1,
            ..
        }
    ));
    assert_eq!(client.phase(), Phase::PairSent);
    assert!(
        client.snapshot(&Snapshot::new(snapshot(1)), None).is_none(),
        "no data before pairing"
    );

    let (frames, state) = device.command("CONFIRM 1");
    let (events, _) = deliver(&mut client, &frames);
    assert_eq!(
        events,
        vec![Event::Paired {
            device_identity: DEVICE
        }]
    );
    assert!(state.paired);
    assert_eq!(state.phase, OPEN);

    let frame = client
        .snapshot(&Snapshot::new(snapshot(437)), None)
        .unwrap();
    let (_, state) = device.push(&frame);
    assert!(state.seen);
    assert_eq!(state.cpu, 437);
}

#[test]
fn paired_host_reconnects_without_touch_on_a_new_session() {
    let mut device = Device::spawn();
    let mut first = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut first);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut first, &frames);
    let (_, state) = device.push(&first.snapshot(&Snapshot::new(snapshot(300)), None).unwrap());
    assert_eq!(state.cpu, 300);

    let mut second = HostClient::new(HOST, Some(DEVICE), CODE, SESSION_B);
    let (events, state) = handshake(&mut device, &mut second);
    assert_eq!(state.phase, OPEN, "no touch needed for a paired Host");
    assert!(events.contains(&Event::Paired {
        device_identity: DEVICE
    }));

    // The previous session's snapshots are rejected; the new one's are not.
    let (_, state) = device.push(&first.snapshot(&Snapshot::new(snapshot(999)), None).unwrap());
    assert_eq!(state.cpu, 300);
    let (_, state) = device.push(
        &second
            .snapshot(&Snapshot::new(snapshot(650)), None)
            .unwrap(),
    );
    assert_eq!(state.cpu, 650);
}

#[test]
fn declined_pairing_is_refused_and_stores_nothing() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, state) = device.command("CONFIRM 0");
    let (events, _) = deliver(&mut client, &frames);
    assert_eq!(events, vec![Event::Refused(Refusal::Declined)]);
    assert!(!state.paired);
    assert_eq!(state.phase, NEGOTIATED);
}

#[test]
fn touch_timeout_declines() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, state) = device.command("TICK 60000");
    let (events, _) = deliver(&mut client, &frames);
    assert_eq!(events, vec![Event::Refused(Refusal::Declined)]);
    assert!(!state.paired);
}

#[test]
fn a_second_host_is_refused_by_a_paired_device() {
    let mut device = Device::spawn();
    let mut owner = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut owner);
    device.command("CONFIRM 1");

    let mut stranger = HostClient::new([0xA2; 16], None, 654_321, SESSION_B);
    let (events, state) = handshake(&mut device, &mut stranger);
    assert!(events.contains(&Event::Refused(Refusal::NotPairedWithYou)));
    assert!(state.paired);
    assert!(stranger
        .snapshot(&Snapshot::new(snapshot(1)), None)
        .is_none());
}

#[test]
fn incompatible_major_is_refused_and_nothing_is_applied() {
    let mut device = Device::spawn();
    let hello = encode_control(
        &Envelope {
            major: 1,
            minor: 0,
            kind: MessageType::Hello,
            session: [0; 16],
            sequence: 1,
            utc_ms: None,
            utc_uncertainty_ms: None,
            local_offset_minutes: None,
        },
        &ControlBody::Hello(HelloBody {
            supported_major: 2,
            supported_minor: 0,
            capabilities: 0,
            configuration_revision: 0,
            utc_ms: None,
            utc_uncertainty_ms: None,
        }),
    )
    .unwrap();
    let (frames, state) = device.push(&hello);
    assert_eq!(state.phase, INCOMPATIBLE);
    assert!(!state.seen);
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    client.start(None).unwrap();
    let (events, _) = deliver(&mut client, &frames);
    assert_eq!(events, vec![Event::Refused(Refusal::Incompatible)]);
}

#[test]
fn unpair_and_calibration_reset_are_idempotent() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut client, &frames);

    let (id, frame) = client.command(CommandOperation::CalibrationReset).unwrap();
    let (frames, _) = device.push(&frame);
    let (events, _) = deliver(&mut client, &frames);
    let reply = |result| Event::CommandReply {
        command_identity: id,
        operation: CommandOperation::CalibrationReset,
        result,
    };
    assert_eq!(events, vec![reply(CommandResult::Applied)]);
    let retry = client
        .retry_command(id, CommandOperation::CalibrationReset)
        .unwrap();
    let (frames, _) = device.push(&retry);
    let (events, _) = deliver(&mut client, &frames);
    assert_eq!(events, vec![reply(CommandResult::AlreadyApplied)]);

    let (unpair_id, frame) = client.command(CommandOperation::Unpair).unwrap();
    let (frames, state) = device.push(&frame);
    let (events, _) = deliver(&mut client, &frames);
    assert_eq!(
        events,
        vec![Event::CommandReply {
            command_identity: unpair_id,
            operation: CommandOperation::Unpair,
            result: CommandResult::Applied
        }]
    );
    assert!(!state.paired);
    assert_eq!(state.phase, NEGOTIATED);
}

#[test]
fn a_ram_group_reaches_the_firmware_state_and_may_stop_being_sent() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut client, &frames);

    let ram = RamSnapshot {
        availability: Availability::Available,
        used_bytes: 8 << 30,
        total_bytes: 16 << 30,
        source_age_ms: 250,
        freshness: Freshness::Fresh,
    };
    let frame = client
        .snapshot(
            &Snapshot {
                ram: Some(ram.clone()),
                ..Snapshot::new(snapshot(400))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!((state.cpu, state.ram), (400, Some((8 << 30, 16 << 30))));

    // A later snapshot without the group keeps the last RAM reading (it
    // goes stale on its own clock) while CPU updates.
    let frame = client
        .snapshot(&Snapshot::new(snapshot(500)), None)
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!((state.cpu, state.ram), (500, Some((8 << 30, 16 << 30))));
}

#[test]
fn a_storage_group_reaches_the_firmware_state_beside_ram() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut client, &frames);

    let ram = RamSnapshot {
        availability: Availability::Available,
        used_bytes: 8 << 30,
        total_bytes: 16 << 30,
        source_age_ms: 0,
        freshness: Freshness::Fresh,
    };
    let storage = StorageSnapshot {
        capacity_availability: Availability::Available,
        total_bytes: 500_000_000_000,
        available_bytes: 200_000_000_000,
        capacity_age_ms: 1500,
        io_availability: Availability::Available,
        read_bytes_per_s: 12_300_000,
        write_bytes_per_s: 4_500_000,
        io_age_ms: 0,
    };
    let frame = client
        .snapshot(
            &Snapshot {
                ram: Some(ram.clone()),
                storage: Some(storage.clone()),
                ..Snapshot::new(snapshot(400))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(state.ram, Some((8 << 30, 16 << 30)));
    assert_eq!(
        state.storage,
        Some([500_000_000_000, 200_000_000_000, 12_300_000, 4_500_000])
    );
}

#[test]
fn a_battery_group_reaches_the_firmware_state_and_absence_is_not_zero() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut client, &frames);

    let battery = BatterySnapshot {
        charge_availability: Availability::Available,
        charge_per_mille: 870,
        state: 1,
        time_remaining_min: 95,
        charge_age_ms: 250,
        condition_availability: Availability::Available,
        max_capacity_per_mille: 910,
        condition: 1,
        cycle_count: 312,
        condition_age_ms: 1_800_000,
    };
    let frame = client
        .snapshot(
            &Snapshot {
                battery: Some(battery.clone()),
                ..Snapshot::new(snapshot(400))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(state.battery, Some([0, 870, 1, 910]));

    // A batteryless host: both halves absent, values zero.
    let absent = BatterySnapshot {
        charge_availability: Availability::Absent,
        charge_per_mille: 0,
        state: 0,
        time_remaining_min: protocol::BATTERY_UNKNOWN_U16,
        charge_age_ms: 0,
        condition_availability: Availability::Absent,
        max_capacity_per_mille: 0,
        condition: 0,
        cycle_count: protocol::BATTERY_UNKNOWN_U16,
        condition_age_ms: 0,
    };
    let frame = client
        .snapshot(
            &Snapshot {
                battery: Some(absent.clone()),
                ..Snapshot::new(snapshot(500))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(state.battery, Some([2, 0, 0, 0]));
}

#[test]
fn an_agents_group_reaches_the_firmware_state_and_unavailable_is_not_none_running() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut client, &frames);

    let agents = AgentsSnapshot {
        availability: Availability::Available,
        source_age_ms: 250,
        state: [0, 1, 0],
    };
    let frame = client
        .snapshot(
            &Snapshot {
                agents: Some(agents.clone()),
                ..Snapshot::new(snapshot(400))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(state.agents, Some([0, 0, 1, 0]));

    let down = AgentsSnapshot {
        availability: Availability::TemporarilyUnavailable,
        source_age_ms: 0,
        state: [0, 0, 0],
    };
    let frame = client
        .snapshot(
            &Snapshot {
                agents: Some(down.clone()),
                ..Snapshot::new(snapshot(500))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(state.agents, Some([3, 0, 0, 0]));
}

#[test]
fn a_network_group_reaches_the_firmware_state_and_context_is_independent_of_rates() {
    let mut device = Device::spawn();
    let mut client = HostClient::new(HOST, None, CODE, SESSION_A);
    handshake(&mut device, &mut client);
    let (frames, _) = device.command("CONFIRM 1");
    deliver(&mut client, &frames);

    let network = NetworkSnapshot {
        traffic_availability: Availability::Available,
        traffic_age_ms: 0,
        route_kind: RouteKind::Wifi,
        via_tunnel: true,
        rx_bytes_per_s: 1_200_000,
        tx_bytes_per_s: 340_000,
        context_availability: AssociationContext::Denied,
        context_age_ms: 100,
        coverage: WifiCoverage::OutsideSupported,
        channel: 149,
        station: Some([2, 0, 0, 0, 0, 1]),
        ssid: Vec::new(),
        bssid: None,
        links: Some(vec![RouteKind::Wifi, RouteKind::Ethernet]),
    };
    let frame = client
        .snapshot(
            &Snapshot {
                network: Some(network.clone()),
                ..Snapshot::new(snapshot(400))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(
        state.network,
        Some([0, 1, 1, 1_200_000, 340_000, 2, 2, 149])
    );

    // Rates temporarily unavailable (a route change) while the context stays.
    let pending = NetworkSnapshot {
        traffic_availability: Availability::TemporarilyUnavailable,
        rx_bytes_per_s: 0,
        tx_bytes_per_s: 0,
        ..network
    };
    let frame = client
        .snapshot(
            &Snapshot {
                network: Some(pending.clone()),
                ..Snapshot::new(snapshot(500))
            },
            None,
        )
        .unwrap();
    let (_, state) = device.push(&frame);
    assert_eq!(state.network, Some([3, 1, 1, 0, 0, 2, 2, 149]));
}
