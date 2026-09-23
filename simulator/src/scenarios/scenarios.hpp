// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace simulator::scenarios {

// Builds and renders the diagnostic screen with a fixture DiagnosticState,
// asserts expected label text is present. Matches F05's
// `./dev run simulator -- display-smoke`.
int RunDisplaySmoke();

// Drives the calibration UI state machine with synthetic RawTouchSamples
// through all 5 guided targets plus a validation pass, and a deliberate
// validation-failure case, asserting accept/reject transitions. Matches
// F06's `./dev run simulator -- calibration`.
int RunCalibration();

// Renders fixture peripheral rows and asserts diagnostic screen contents.
// Matches F07's `./dev run simulator -- peripherals`.
int RunPeripherals();

// Builds the bounded operational capacity slice (live status, Alert list,
// Alert detail, Settings) from a deliberately oversized fixture, asserting
// that every screen clamps to firmware::domain::capacity's declared row
// and string-length bounds, and that repeated navigation across all four
// screens always leaves exactly one live screen root behind. Matches
// F08a's `./dev run simulator -- capacity`.
int RunCapacity();

// Replays a deterministic negotiated-session transcript through the same C++
// codec and host-state reducer used by firmware. No wall clock or transport is
// consulted, making reconnect/time-discontinuity expectations reproducible.
int RunProtocolReplay();

// Replays CPU snapshot bytes through HostFeed (framing, decode, session gate,
// reducer) with a scripted clock into the CPU screen, asserting each step's
// text against simulator/scenarios/cpu-lifecycle.expected. Matches TB01's
// `./dev run simulator -- cpu-lifecycle`.
int RunCpuLifecycle();

// Runs firmware::domain::CpuReplay's scripted timeline (the same one the board
// replays on `DEV:CPU_REPLAY`) into the CPU screen and compares each
// checkpoint line with simulator/scenarios/cpu-replay.expected.
int RunCpuReplay();

// Feeds snapshots with and without the RAM group through HostFeed and prints
// every registered metric card (firmware/domain/card_registry.def) at each
// step, compared with simulator/scenarios/metric-cards.expected. Matches
// C02's `./dev run simulator -- metric-cards`.
int RunMetricCards();

// Feeds snapshots with the Storage group through HostFeed and prints the
// Storage card at each step (no data, live, I/O unavailable with capacity
// live, counter reset, capacity stale while I/O is live, everything stale),
// compared with simulator/scenarios/storage-cards.expected.
int RunStorageCards();

// Feeds snapshots with the Battery group through HostFeed and prints the
// Battery card at each step (no data, charging, discharging, absent, condition
// unavailable or stale while charge is live, charge error, everything stale),
// compared with simulator/scenarios/battery-cards.expected.
int RunBatteryCards();

// Feeds snapshots with the Agents group through HostFeed and prints the
// Agents card at each step (no data, none running, Claude running with
// activity unknown, two running, source unavailable, future busy/idle states,
// stale, link lost), compared with simulator/scenarios/agents-cards.expected.
int RunAgentsCards();

// Feeds snapshots with the Network group through HostFeed and prints the
// Network card at each step (no data, WiFi with rates, Ethernet, a tunnel
// primary route, a route change, 5/6 GHz outside supported coverage, unknown
// and denied context, provider absent, offline, per-half staleness, link
// lost), compared with simulator/scenarios/network-cards.expected.
int RunNetworkCards();

// Draws the System shell (status bar plus the registered CPU and RAM cards)
// and asserts label text, dimming, and RAM's missing-data behaviour. Matches
// TB03's `./dev run simulator -- system-screen`.
int RunSystemScreen();

// Drives the clock settings panel (24-hour, show timezone, manual zone and
// offset stepping) and its entry point on the status bar, including the
// pairing prompt taking priority. Matches `./dev run simulator -- settings`.
int RunSettings();

// Replays a scripted radio driver (lost windows, a disabled radio, a fault, a
// silent driver, both radios off) through the observation scheduler and
// coverage ledger, comparing each checkpoint with
// simulator/scenarios/coverage.expected. Matches TB09's
// `./dev run simulator -- coverage`.
int RunCoverage();

}  // namespace simulator::scenarios
