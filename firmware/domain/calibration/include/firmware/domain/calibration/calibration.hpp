// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace firmware::domain::calibration {

// A raw sample straight from the XPT2046 touch controller, in its own
// uncalibrated coordinate space. Units are controller ADC counts, not
// screen pixels.
struct RawTouchSample {
  int32_t raw_x = 0;
  int32_t raw_y = 0;
};

// A point in logical (post-rotation) screen pixels, per the 320x240
// landscape geometry firmware/domain/geometry.hpp validates.
struct ScreenPoint {
  int32_t x = 0;
  int32_t y = 0;
};

// The five guided calibration targets, in landscape screen space, inset
// from the panel edges by a 20px margin. Order matters: the guided flow in
// firmware/ui/calibration presents targets to the operator in exactly this
// sequence (top-left, top-right, bottom-right, bottom-left, then center),
// and ComputeAffineTransform expects raw samples in this same order.
inline constexpr int32_t kCalibrationMarginPx = 20;
inline constexpr std::array<ScreenPoint, 5> kCalibrationTargets = {{
    {kCalibrationMarginPx, kCalibrationMarginPx},              // 0: top-left
    {320 - kCalibrationMarginPx, kCalibrationMarginPx},        // 1: top-right
    {320 - kCalibrationMarginPx, 240 - kCalibrationMarginPx},  // 2: bottom-right
    {kCalibrationMarginPx, 240 - kCalibrationMarginPx},        // 3: bottom-left
    {160, 120},                                                // 4: center
}};

// Maps raw touch samples to screen pixels:
//   screen.x = a*raw.x + b*raw.y + c
//   screen.y = d*raw.x + e*raw.y + f
struct AffineTransform {
  double a = 0;
  double b = 0;
  double c = 0;
  double d = 0;
  double e = 0;
  double f = 0;
};

// Least-squares fit of a 3-parameter-per-axis affine transform from five
// guided (raw, target) point pairs. Five points over-determine the 3
// unknowns per axis -- that redundancy is the point of guided 5-point
// calibration versus a minimal 3-point fit, and it is what lets this
// function reject a degenerate fit instead of just interpolating exactly
// through noise. Returns std::nullopt if the raw points are degenerate
// (e.g. collinear, or otherwise producing a near-singular normal-equations
// matrix) rather than producing a garbage transform.
std::optional<AffineTransform> ComputeAffineTransform(const std::array<RawTouchSample, 5>& raw,
                                                      const std::array<ScreenPoint, 5>& targets);

// Applies a computed transform to a raw sample, producing a screen point.
ScreenPoint ApplyTransform(const AffineTransform& transform, RawTouchSample raw);

// Result of validating one observed tap against its expected screen
// location after calibration.
struct ValidationResult {
  bool accepted = false;
  double error_px = 0;
};

// Provisional default maximum acceptable validation error, in logical
// screen pixels, on the 320x240 panel. This is a placeholder pending F08's
// physical measurement on real hardware -- it is not derived from any
// measured touch data.
inline constexpr double kDefaultMaxValidationErrorPx = 15.0;

// Applies `transform` to `observed`, then checks its Euclidean distance to
// `expected` against `max_error_px`.
ValidationResult ValidateTap(const AffineTransform& transform, RawTouchSample observed,
                             ScreenPoint expected,
                             double max_error_px = kDefaultMaxValidationErrorPx);

// The current on-device calibration record schema. Bump this whenever the
// record's field layout or the landscape coordinate model changes; loaders
// must treat a schema mismatch as "not calibrated," not as a compatible
// record to coerce.
inline constexpr uint32_t kCalibrationSchemaVersion = 1;

// A calibration record as persisted to NVS. `checksum` covers every other
// field (see ComputeChecksum) together with the identity fields, so a
// torn, incompatible, or bit-corrupted record is detected on load rather
// than silently accepted -- see IsRecordValid.
struct CalibrationRecord {
  std::string profile_id;
  std::string orientation;
  uint32_t schema_version = kCalibrationSchemaVersion;
  AffineTransform transform;
  uint32_t checksum = 0;
};

// Computes a CRC32 over `record`'s profile_id, orientation, schema_version,
// and the six transform doubles' bit patterns. Does not read or depend on
// `record.checksum` itself.
uint32_t ComputeChecksum(const CalibrationRecord& record);

// Recomputes the checksum over `record` and compares it to the stored one,
// and separately checks profile_id/orientation/schema_version against the
// caller's expected identity. A record that fails either check is not
// usable: torn, incompatible-profile, incompatible-orientation, or
// corrupt-transform records must all return the caller to calibration
// rather than accepting bad coordinates.
bool IsRecordValid(const CalibrationRecord& record, std::string_view expected_profile_id,
                   std::string_view expected_orientation, uint32_t expected_schema_version);

}  // namespace firmware::domain::calibration
