// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "freertos/FreeRTOS.h"

namespace firmware::platform::esp32::radio {

// EventRing lock policy: a portMUX critical section (interrupts masked on this
// core, spinlock against the other). Held only for a bounded copy: nothing
// that allocates, logs or blocks may run while it is taken.
class PortMuxLock {
 public:
  void lock() { portENTER_CRITICAL(&mux_); }
  void unlock() { portEXIT_CRITICAL(&mux_); }

 private:
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace firmware::platform::esp32::radio
