// SPDX-License-Identifier: Apache-2.0
#include "expansion/expansion.hpp"

#include "driver/gpio.h"
#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::expansion {

bool Init() {
  const gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << firmware::domain::profile::kExpansionInputOnly,
      .mode = GPIO_MODE_INPUT,
      // GPIO35 is one of the ESP32's input-only pins (32-39 range) with no
      // internal pull-up/pull-down hardware at all; requesting one here
      // would be silently ineffective. See touch/touch.hpp's IRQ pin for
      // the same constraint.
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&cfg) == ESP_OK;
}

bool IsHigh() {
  return gpio_get_level(static_cast<gpio_num_t>(firmware::domain::profile::kExpansionInputOnly)) !=
         0;
}

}  // namespace firmware::platform::esp32::expansion
