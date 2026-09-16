// SPDX-License-Identifier: Apache-2.0
#include "audio/audio.hpp"

#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::audio {

namespace {

dac_oneshot_handle_t g_dac = nullptr;

gpio_num_t EnableGpio() { return static_cast<gpio_num_t>(firmware::domain::profile::kAudioEnable); }

void SetEnablePin(bool enabled) {
  const bool level = firmware::domain::profile::kAudioEnableActiveLow ? !enabled : enabled;
  gpio_set_level(EnableGpio(), level);
}

}  // namespace

bool Init() {
  const gpio_config_t enable_cfg = {
      .pin_bit_mask = 1ULL << firmware::domain::profile::kAudioEnable,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&enable_cfg) != ESP_OK) return false;
  SetEnablePin(false);  // alert_sound_default is off; start disabled

  dac_oneshot_config_t dac_cfg = {};
  dac_cfg.chan_id = firmware::domain::profile::kAudioDac == 26 ? DAC_CHAN_1 : DAC_CHAN_0;
  if (dac_oneshot_new_channel(&dac_cfg, &g_dac) != ESP_OK) return false;

  return true;
}

void SetAmplifierEnabled(bool enabled) { SetEnablePin(enabled); }

void PlayTestTone(uint32_t frequency_hz, uint32_t duration_ms) {
  if (g_dac == nullptr || frequency_hz == 0) return;

  const uint32_t half_period_us = 1000000u / (2u * frequency_hz);
  const uint32_t cycles = (duration_ms * 1000u) / (2u * half_period_us);

  for (uint32_t i = 0; i < cycles; ++i) {
    dac_oneshot_output_voltage(g_dac, 200);  // simple square wave, not a calibrated level
    esp_rom_delay_us(half_period_us);
    dac_oneshot_output_voltage(g_dac, 0);
    esp_rom_delay_us(half_period_us);
  }
}

}  // namespace firmware::platform::esp32::audio
