// SPDX-License-Identifier: Apache-2.0
#include "nvs/display_settings_store.hpp"

#include "nvs.h"
#include "nvs_flash.h"

namespace firmware::platform::esp32::nvs {
namespace {
constexpr const char* kNamespace = "fw_display";
constexpr const char* kKey = "settings";
}  // namespace

firmware::domain::DisplaySettings LoadDisplaySettings() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return {};
  firmware::domain::SettingsRecord record{};
  size_t size = record.size();
  const bool ok =
      nvs_get_blob(handle, kKey, record.data(), &size) == ESP_OK && size == record.size();
  nvs_close(handle);
  if (!ok) return {};
  return firmware::domain::Deserialize(record).value_or(firmware::domain::DisplaySettings{});
}

bool SaveDisplaySettings(const firmware::domain::DisplaySettings& settings) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const auto record = firmware::domain::Serialize(settings);
  const bool ok = nvs_set_blob(handle, kKey, record.data(), record.size()) == ESP_OK &&
                  nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

}  // namespace firmware::platform::esp32::nvs
