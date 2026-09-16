// SPDX-License-Identifier: Apache-2.0
#include "led/led.hpp"

#include "driver/ledc.h"
#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::led {

namespace {

constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;  // distinct from display backlight's LEDC_TIMER_0
constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_8_BIT;  // 0..255, matches SetColor's range

struct Channel {
  ledc_channel_t channel;
  int gpio;
};

const Channel kChannels[3] = {
    {LEDC_CHANNEL_1, firmware::domain::profile::kRgbLedRed},
    {LEDC_CHANNEL_2, firmware::domain::profile::kRgbLedGreen},
    {LEDC_CHANNEL_3, firmware::domain::profile::kRgbLedBlue},
};

uint8_t ToDuty(uint8_t brightness) {
  // Common-anode active-low wiring: full brightness is duty 0 (pin held
  // low), off is duty 255 (pin held high).
  return firmware::domain::profile::kRgbLedActiveLow ? static_cast<uint8_t>(255 - brightness)
                                                     : brightness;
}

}  // namespace

bool Init() {
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = kSpeedMode;
  timer_cfg.timer_num = kTimer;
  timer_cfg.duty_resolution = kDutyResolution;
  timer_cfg.freq_hz = 5000;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer_cfg) != ESP_OK) return false;

  for (const auto& ch : kChannels) {
    ledc_channel_config_t channel_cfg = {};
    channel_cfg.gpio_num = ch.gpio;
    channel_cfg.speed_mode = kSpeedMode;
    channel_cfg.channel = ch.channel;
    channel_cfg.timer_sel = kTimer;
    channel_cfg.duty = ToDuty(0);  // start off
    channel_cfg.hpoint = 0;
    if (ledc_channel_config(&channel_cfg) != ESP_OK) return false;
  }
  return true;
}

void SetColor(uint8_t red, uint8_t green, uint8_t blue) {
  const uint8_t duties[3] = {ToDuty(red), ToDuty(green), ToDuty(blue)};
  for (int i = 0; i < 3; ++i) {
    ledc_set_duty(kSpeedMode, kChannels[i].channel, duties[i]);
    ledc_update_duty(kSpeedMode, kChannels[i].channel);
  }
}

}  // namespace firmware::platform::esp32::led
