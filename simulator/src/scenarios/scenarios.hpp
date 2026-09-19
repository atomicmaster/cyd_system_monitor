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

}  // namespace simulator::scenarios
