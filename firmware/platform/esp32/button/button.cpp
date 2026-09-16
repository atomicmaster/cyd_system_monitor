// SPDX-License-Identifier: Apache-2.0
#include "button/button.hpp"

#include "driver/gpio.h"

#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::button {

bool Init() {
  const gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << firmware::domain::profile::kButtonDownload,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&cfg) == ESP_OK;
}

bool IsPressed() {
  return gpio_get_level(static_cast<gpio_num_t>(firmware::domain::profile::kButtonDownload)) == 0;
}

}  // namespace firmware::platform::esp32::button
