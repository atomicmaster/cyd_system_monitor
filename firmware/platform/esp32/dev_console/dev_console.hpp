// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::platform::esp32::dev_console {

// Development-only recovery/diagnostic path; TB02 replaces this with the
// negotiated product USB command. Starts a FreeRTOS task that reads ASCII
// lines from UART0 (profile uart0.rx/tx, the same USB-serial bridge used
// for ESP_LOGI output) and dispatches exact-match commands: F06's
// "DEV:CLEAR_CALIBRATION" (clears the stored calibration record), and
// F07's on-demand hardware exercises "DEV:LED_TEST" (cycles red/green/
// blue), "DEV:AUDIO_TEST" (deliberately enables the amplifier and plays a
// tone), and "DEV:PERIPHERAL_STATUS" (re-probes and logs MicroSD/battery/
// BOOT-button state live). Bounded and narrow on purpose: this is not
// TB02's real USB session/protocol, just a way to exercise/recover a
// board before that protocol exists.
void StartDevConsole();

}  // namespace firmware::platform::esp32::dev_console
