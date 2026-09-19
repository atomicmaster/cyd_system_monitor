// SPDX-License-Identifier: Apache-2.0
#include "nvs/pairing_store.hpp"

#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace firmware::platform::esp32::nvs {
namespace {

constexpr const char* kNamespace = "fw_pair";
constexpr const char* kDeviceKey = "device";
constexpr const char* kHostKey = "host";

bool ReadIdentity(const char* key, firmware::domain::Identity& out) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t size = out.size();
  const bool ok = nvs_get_blob(handle, key, out.data(), &size) == ESP_OK && size == out.size();
  nvs_close(handle);
  return ok;
}

bool WriteIdentity(const char* key, const firmware::domain::Identity& value) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = nvs_set_blob(handle, key, value.data(), value.size()) == ESP_OK &&
                  nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

}  // namespace

firmware::domain::Identity NvsPairingStore::DeviceIdentity() {
  firmware::domain::Identity identity{};
  if (ReadIdentity(kDeviceKey, identity)) return identity;
  esp_fill_random(identity.data(), identity.size());
  WriteIdentity(kDeviceKey, identity);
  return identity;
}

std::optional<firmware::domain::Identity> NvsPairingStore::PairedHost() {
  firmware::domain::Identity host{};
  if (!ReadIdentity(kHostKey, host)) return std::nullopt;
  return host;
}

bool NvsPairingStore::SavePairedHost(const firmware::domain::Identity& host) {
  return WriteIdentity(kHostKey, host);
}

bool NvsPairingStore::ClearPairedHost() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const esp_err_t erased = nvs_erase_key(handle, kHostKey);
  const bool ok =
      (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

}  // namespace firmware::platform::esp32::nvs
