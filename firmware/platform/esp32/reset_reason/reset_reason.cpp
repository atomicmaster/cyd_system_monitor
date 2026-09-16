// SPDX-License-Identifier: Apache-2.0
#include "reset_reason/reset_reason.hpp"

#include "esp_system.h"

namespace firmware::platform::esp32::reset_reason {

firmware::domain::ResetReason ReadResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return firmware::domain::ResetReason::PowerOn;
    case ESP_RST_EXT:
      return firmware::domain::ResetReason::ExternalReset;
    case ESP_RST_SW:
      return firmware::domain::ResetReason::Software;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return firmware::domain::ResetReason::Watchdog;
    case ESP_RST_BROWNOUT:
      return firmware::domain::ResetReason::Brownout;
    case ESP_RST_DEEPSLEEP:
      return firmware::domain::ResetReason::DeepSleepWake;
    case ESP_RST_SDIO:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
    case ESP_RST_EFUSE:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_UNKNOWN:
    default:
      return firmware::domain::ResetReason::Unknown;
  }
}

}  // namespace firmware::platform::esp32::reset_reason
