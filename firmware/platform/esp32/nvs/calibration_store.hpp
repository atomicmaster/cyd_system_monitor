// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware/domain/calibration/calibration.hpp"

namespace firmware::platform::esp32::nvs {

// Calibration record persistence backed by the nvs_flash/nvs C API,
// storing firmware::domain::calibration::CalibrationRecord as a blob.
//
// Every load path calls firmware::domain::calibration::IsRecordValid and
// treats any failure -- missing key, size mismatch, or checksum mismatch
// -- as "not calibrated" rather than crashing or accepting a torn record.
// This directly implements F06's acceptance line "torn, incompatible, or
// corrupt records return to calibration rather than accepting bad
// coordinates."

// Loads and validates the stored record against `expected_profile_id`/
// `expected_orientation`/`expected_schema_version`. Returns true and fills
// `out` only if a record exists, its stored size matches the expected
// blob layout, and IsRecordValid() accepts it.
bool LoadCalibrationRecord(firmware::domain::calibration::CalibrationRecord& out,
                           const char* expected_profile_id, const char* expected_orientation,
                           uint32_t expected_schema_version);

// Persists `record` as a blob. Returns false on any NVS error; the caller
// must not assume calibration survived a failed save.
bool SaveCalibrationRecord(const firmware::domain::calibration::CalibrationRecord& record);

// Erases the stored calibration record, if any. Used both by the
// diagnostics recalibration entrypoint and the development-only USB
// clear-calibration command (see dev_console/dev_console.hpp).
void ClearCalibrationRecord();

}  // namespace firmware::platform::esp32::nvs
