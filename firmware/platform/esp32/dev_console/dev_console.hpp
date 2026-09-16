// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::platform::esp32::dev_console {

// Development-only recovery path; TB02 replaces this with the negotiated
// product USB command. Starts a FreeRTOS task that reads ASCII lines from
// UART0 (profile uart0.rx/tx, the same USB-serial bridge used for
// ESP_LOGI output) and, on an exact "DEV:CLEAR_CALIBRATION" line, calls
// firmware::platform::esp32::nvs::ClearCalibrationRecord(). Bounded and
// narrow on purpose: this is not TB02's real USB session/protocol, just a
// way to recover a board whose touch calibration is unusable before that
// protocol exists.
void StartDevConsole();

}  // namespace firmware::platform::esp32::dev_console
