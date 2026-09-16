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

}  // namespace simulator::scenarios
