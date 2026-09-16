// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware/domain/calibration/calibration.hpp"

namespace firmware::platform::esp32::touch {

// XPT2046 resistive touch driver.
//
// PROVISIONAL: implemented as software/bit-banged GPIO SPI, not a hardware
// SPI peripheral. Display, touch, and MicroSD are wired to three separate
// SPI buses on this board, but ESP32 exposes only two general-purpose SPI
// controllers (see hardware/profiles/lcdwiki-esp32-32e-2.8/README.md's
// "Observation and feasibility limits" section). F06 picks bit-banged
// touch SPI as the provisional resolution so the display keeps its own
// hardware SPI2 bus undisturbed; F08 measures and finalizes the real
// arrangement under combined load.
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

 private:
  bool initialized_ = false;
};

}  // namespace firmware::platform::esp32::touch
