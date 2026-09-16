// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL bit-banged XPT2046 driver -- see touch.hpp and
// hardware/profiles/lcdwiki-esp32-32e-2.8/README.md's open
// three-buses-vs-two-controllers question. F08 validates or replaces this
// under combined load.
#include "touch/touch.hpp"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::touch {

namespace {

// Standard XPT2046 control-byte command bytes: start bit, channel select
// (Y=1,X=5), 12-bit mode, single-ended, power-down-between-conversions.
constexpr uint8_t kCommandReadY = 0x90;
constexpr uint8_t kCommandReadX = 0xD0;

constexpr int kBitBangDelayUs = 2;  // conservative clock half-period for bit-banged SPI
constexpr int kSampleCount =
    4;  // averaged reads per axis, per touch.hpp's documented noise reduction

void ClockDelay() { esp_rom_delay_us(kBitBangDelayUs); }

void SetSclk(bool level) {
  gpio_set_level(static_cast<gpio_num_t>(firmware::domain::profile::kTouchSclk), level);
}
void SetMosi(bool level) {
  gpio_set_level(static_cast<gpio_num_t>(firmware::domain::profile::kTouchMosi), level);
}
void SetCs(bool level) {
  gpio_set_level(static_cast<gpio_num_t>(firmware::domain::profile::kTouchCs), level);
}
int ReadMiso() {
  return gpio_get_level(static_cast<gpio_num_t>(firmware::domain::profile::kTouchMiso));
}

// Bit-bangs one 8-bit command out and 12 clocked-in result bits back
// (XPT2046 returns a 12-bit sample left-justified in the first 12 clocks
// after the command byte, per its datasheet timing diagram).
uint16_t TransferCommand(uint8_t command) {
  SetCs(false);

  for (int bit = 7; bit >= 0; --bit) {
    SetMosi((command >> bit) & 0x1);
    ClockDelay();
    SetSclk(true);
    ClockDelay();
    SetSclk(false);
  }

  uint16_t result = 0;
  for (int bit = 0; bit < 12; ++bit) {
    ClockDelay();
    SetSclk(true);
    ClockDelay();
    result = static_cast<uint16_t>((result << 1) | ReadMiso());
    SetSclk(false);
  }

  SetCs(true);
  return result;
}

int32_t AveragedRead(uint8_t command) {
  int32_t sum = 0;
  for (int i = 0; i < kSampleCount; ++i) {
    sum += TransferCommand(command);
  }
  return sum / kSampleCount;
}

}  // namespace

bool Xpt2046Touch::Init() {
  const gpio_config_t output_cfg = {
      .pin_bit_mask = (1ULL << firmware::domain::profile::kTouchSclk) |
                      (1ULL << firmware::domain::profile::kTouchMosi) |
                      (1ULL << firmware::domain::profile::kTouchCs),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&output_cfg) != ESP_OK) return false;

  const gpio_config_t input_cfg = {
      .pin_bit_mask = (1ULL << firmware::domain::profile::kTouchMiso) |
                      (1ULL << firmware::domain::profile::kTouchIrq),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&input_cfg) != ESP_OK) return false;

  SetCs(true);
  SetSclk(false);
  initialized_ = true;
  return true;
}

bool Xpt2046Touch::ReadRaw(firmware::domain::calibration::RawTouchSample& out) {
  if (!initialized_) return false;
  if (!IrqAsserted()) return false;

  out.raw_x = AveragedRead(kCommandReadX);
  out.raw_y = AveragedRead(kCommandReadY);
  return true;
}

bool Xpt2046Touch::IrqAsserted() const {
  // IRQ is active-low while the panel is pressed.
  return gpio_get_level(static_cast<gpio_num_t>(firmware::domain::profile::kTouchIrq)) == 0;
}

firmware::domain::calibration::RawTouchSample Xpt2046Touch::ReadRawIgnoringIrq() {
  firmware::domain::calibration::RawTouchSample out;
  out.raw_x = AveragedRead(kCommandReadX);
  out.raw_y = AveragedRead(kCommandReadY);
  return out;
}

}  // namespace firmware::platform::esp32::touch
