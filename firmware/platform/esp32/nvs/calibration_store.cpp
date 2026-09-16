// SPDX-License-Identifier: Apache-2.0
#include "nvs/calibration_store.hpp"

#include <cstring>

#include "nvs.h"
#include "nvs_flash.h"

namespace firmware::platform::esp32::nvs {

namespace {

constexpr const char* kNamespace = "fw_calib";
constexpr const char* kKey = "record";

// Fixed-layout on-disk representation: CalibrationRecord's std::string
// fields aren't a stable blob layout, so this struct is the actual NVS
// blob shape. Field order/sizes here are part of the calibration schema;
// changing them must bump firmware::domain::calibration::kCalibrationSchemaVersion.
struct StoredRecord {
  char profile_id[40];
  char orientation[16];
  uint32_t schema_version;
  double transform[6];
  uint32_t checksum;
};

bool ToStored(const firmware::domain::calibration::CalibrationRecord& record, StoredRecord& out) {
  if (record.profile_id.size() >= sizeof(out.profile_id)) return false;
  if (record.orientation.size() >= sizeof(out.orientation)) return false;

  std::memset(&out, 0, sizeof(out));
  std::memcpy(out.profile_id, record.profile_id.c_str(), record.profile_id.size());
  std::memcpy(out.orientation, record.orientation.c_str(), record.orientation.size());
  out.schema_version = record.schema_version;
  out.transform[0] = record.transform.a;
  out.transform[1] = record.transform.b;
  out.transform[2] = record.transform.c;
  out.transform[3] = record.transform.d;
  out.transform[4] = record.transform.e;
  out.transform[5] = record.transform.f;
  out.checksum = record.checksum;
  return true;
}

firmware::domain::calibration::CalibrationRecord FromStored(const StoredRecord& stored) {
  firmware::domain::calibration::CalibrationRecord record;
  // profile_id/orientation are NUL-padded fixed buffers, not necessarily
  // NUL-terminated if a future field grows to fill the buffer exactly;
  // bound the string construction by the buffer size either way.
  record.profile_id.assign(stored.profile_id, strnlen(stored.profile_id, sizeof(stored.profile_id)));
  record.orientation.assign(stored.orientation, strnlen(stored.orientation, sizeof(stored.orientation)));
  record.schema_version = stored.schema_version;
  record.transform = {stored.transform[0], stored.transform[1], stored.transform[2],
                       stored.transform[3], stored.transform[4], stored.transform[5]};
  record.checksum = stored.checksum;
  return record;
}

}  // namespace

bool LoadCalibrationRecord(firmware::domain::calibration::CalibrationRecord& out,
                            const char* expected_profile_id, const char* expected_orientation,
                            uint32_t expected_schema_version) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;  // no namespace yet: not calibrated, not an error
  }

  StoredRecord stored{};
  size_t required_size = sizeof(stored);
  esp_err_t err = nvs_get_blob(handle, kKey, &stored, &required_size);
  nvs_close(handle);

  if (err != ESP_OK) {
    return false;  // missing key: not calibrated
  }
  if (required_size != sizeof(stored)) {
    return false;  // torn/incompatible-size record: treat as not calibrated
  }

  const auto candidate = FromStored(stored);
  if (!firmware::domain::calibration::IsRecordValid(candidate, expected_profile_id, expected_orientation,
                                                      expected_schema_version)) {
    return false;  // corrupt or incompatible: return to calibration
  }

  out = candidate;
  return true;
}

bool SaveCalibrationRecord(const firmware::domain::calibration::CalibrationRecord& record) {
  StoredRecord stored{};
  if (!ToStored(record, stored)) return false;

  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;

  esp_err_t err = nvs_set_blob(handle, kKey, &stored, sizeof(stored));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

void ClearCalibrationRecord() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_erase_key(handle, kKey);  // ESP_ERR_NVS_NOT_FOUND is fine: already clear
  nvs_commit(handle);
  nvs_close(handle);
}

}  // namespace firmware::platform::esp32::nvs
