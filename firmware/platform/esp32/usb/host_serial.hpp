// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace firmware::platform::esp32::usb {

// The negotiated Host link over the board's USB-serial bridge (UART0, the
// same port as logs and the dev console). The Host switches the port from
// the ASCII console to binary frames by sending the text line "CYD:LINK";
// from then on log output is silenced, line-ending translation is off, and
// every received byte is queued for the app loop. Leaves link mode only on
// reset.
//
// The dev console task owns the blocking read; it calls EnterLinkMode()
// and then feeds bytes with OnRxByte(). Everything else runs on the app
// loop, which drains with Read() and answers with Write().
void EnterLinkMode();
bool LinkModeActive();

// Queues one received byte (dev-console task only).
void OnRxByte(uint8_t byte);

// Copies up to `capacity` queued bytes without blocking; returns the count.
size_t Read(uint8_t* out, size_t capacity);

// Writes a whole frame to the Host.
void Write(std::span<const uint8_t> bytes);

}  // namespace firmware::platform::esp32::usb
