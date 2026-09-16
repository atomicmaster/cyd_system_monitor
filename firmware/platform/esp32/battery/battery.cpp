// SPDX-License-Identifier: Apache-2.0
#include "battery/battery.hpp"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::battery {

namespace {

// GPIO34 is ADC1 channel 6 on ESP32. The profile only guarantees the GPIO
// number; this mapping is fixed by the SoC, not configurable.
constexpr adc_channel_t kAdcChannel = ADC_CHANNEL_6;
constexpr adc_unit_t kAdcUnit = ADC_UNIT_1;

adc_oneshot_unit_handle_t g_adc_handle = nullptr;
adc_cali_handle_t g_cali_handle = nullptr;
bool g_calibrated = false;

}  // namespace

bool Init() {
  static_assert(
      firmware::domain::profile::kBatteryAdc == 34,
      "battery ADC pin mapping to ADC_CHANNEL_6 assumes GPIO34; update if the profile changes");

  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = kAdcUnit;
  if (adc_oneshot_new_unit(&unit_cfg, &g_adc_handle) != ESP_OK) return false;

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  chan_cfg.atten = ADC_ATTEN_DB_12;  // full ~0-3.3V range for a supply-voltage diagnostic read
  if (adc_oneshot_config_channel(g_adc_handle, kAdcChannel, &chan_cfg) != ESP_OK) return false;

  // The original ESP32 (this board's chip) only supports the ADC
  // calibration v1 "line fitting" scheme; curve fitting is v2/v3 hardware
  // only available on later chips (S2/C3/S3/...).
  adc_cali_line_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = kAdcUnit;
  cali_cfg.atten = ADC_ATTEN_DB_12;
  cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  g_calibrated = adc_cali_create_scheme_line_fitting(&cali_cfg, &g_cali_handle) == ESP_OK;

  return true;
}

int ReadMillivolts() {
  if (g_adc_handle == nullptr) return -1;

  int raw = 0;
  if (adc_oneshot_read(g_adc_handle, kAdcChannel, &raw) != ESP_OK) return -1;

  if (g_calibrated) {
    int millivolts = 0;
    if (adc_cali_raw_to_voltage(g_cali_handle, raw, &millivolts) == ESP_OK) {
      return millivolts;
    }
  }

  // Uncalibrated fallback: linear approximation over the 12-bit range at
  // ADC_ATTEN_DB_12's ~3300mV full scale. Coarser than the calibrated
  // path but still a usable diagnostic value if line-fitting calibration
  // is unavailable on this chip revision (e.g. no eFuse calibration data).
  return (raw * 3300) / 4095;
}

}  // namespace firmware::platform::esp32::battery
