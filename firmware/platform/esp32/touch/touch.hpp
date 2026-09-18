// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware/domain/calibration/calibration.hpp"

namespace firmware::platform::esp32::touch {

// XPT2046 resistive touch driver.
//
// Implemented as software/bit-banged GPIO SPI, not a hardware SPI
// peripheral. Display, touch, and MicroSD are wired to three separate SPI
// buses on this board, but ESP32 exposes only two general-purpose SPI
// controllers (see hardware/profiles/lcdwiki-esp32-32e-2.8/README.md's
// "Observation and feasibility limits" section). F06 picked bit-banged
// touch SPI so the display keeps its own hardware SPI2 bus undisturbed;
// F08 then measured this arrangement repeatedly under combined
// radio/UI/serial load, including a physical operator actively tapping
// through calibration and the capacity slice, with no observed loss or
// degradation -- see
// hardware/profiles/lcdwiki-esp32-32e-2.8/development/feasibility.md's
// "Required next physical evidence" section. An alternative (hardware-SPI
// touch, bit-banged MicroSD) was evaluated and deliberately not built:
// MicroSD's own hardware-SPI bandwidth only starts mattering once V2's
// SD-backed WiGLE Survey Artifacts writes to it, so that comparison is
// deferred to before that feature, not required for M1a/M2.
class Xpt2046Touch {
 public:
  // Configures the touch controller's GPIOs (profile touch.sclk/mosi/miso/
  // cs/irq) as bit-banged SPI plus an input for IRQ. Returns false if GPIO
  // configuration fails.
  bool Init();

  // Returns true if the panel is currently pressed (IRQ asserted low) and
  // a raw sample was read; false if not pressed. Averages a small number
  // of consecutive raw reads to reduce single-sample resistive-touch noise
  // before handing the result to firmware::domain::calibration.
  bool ReadRaw(firmware::domain::calibration::RawTouchSample& out);

  // DIAGNOSTIC, pending physical F06 evidence: true if the controller's
  // IRQ (T_IRQ) pin currently reads asserted (low). Exposed separately
  // from ReadRaw() so a caller can log/compare it against
  // ReadRawIgnoringIrq()'s result to tell an IRQ-detection fault apart
  // from a SPI-data-path fault when a physical tap produces no response.
  bool IrqAsserted() const;

  // DIAGNOSTIC, pending physical F06 evidence: performs the same averaged
  // X/Y read as ReadRaw() but unconditionally, ignoring IRQ entirely.
  // Never used by the real calibration flow (SubmitRawSample must only
  // see genuine touch-down events, not continuous noise); only for
  // comparing against IrqAsserted() while debugging the real board.
  firmware::domain::calibration::RawTouchSample ReadRawIgnoringIrq();

 private:
  bool initialized_ = false;
};

}  // namespace firmware::platform::esp32::touch
