// SPDX-License-Identifier: Apache-2.0
//
// Proves firmware/domain/calibration builds and runs independently of
// ESP-IDF and any physical touch hardware. This is not a real-touch test;
// see docs/tickets/F06.md for what remains unverified pending hardware.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "firmware/domain/calibration/calibration.hpp"

namespace {

using firmware::domain::calibration::AffineTransform;
using firmware::domain::calibration::ApplyTransform;
using firmware::domain::calibration::CalibrationRecord;
using firmware::domain::calibration::ComputeAffineTransform;
using firmware::domain::calibration::ComputeChecksum;
using firmware::domain::calibration::IsRecordValid;
using firmware::domain::calibration::kCalibrationTargets;
using firmware::domain::calibration::RawTouchSample;
using firmware::domain::calibration::ScreenPoint;
using firmware::domain::calibration::ValidateTap;

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

bool NearlyEqual(double got, double want, double epsilon = 1e-6) {
  return std::fabs(got - want) <= epsilon;
}

// Every raw sample here is target/2 under the known transform
// a=2,b=0,c=0,d=0,e=2,f=0, chosen so integer raw samples land exactly on
// integer targets with no rounding, letting recovery be checked exactly.
std::array<RawTouchSample, 5> HalfScaleRawSamples() {
  std::array<RawTouchSample, 5> raw{};
  for (size_t i = 0; i < kCalibrationTargets.size(); ++i) {
    raw[i] = RawTouchSample{kCalibrationTargets[i].x / 2, kCalibrationTargets[i].y / 2};
  }
  return raw;
}

void TestExactRecoveryFromNoiselessPoints() {
  const auto raw = HalfScaleRawSamples();
  const auto transform = ComputeAffineTransform(raw, kCalibrationTargets);
  Expect(transform.has_value(), "a well-posed 5-point fit produces a transform");
  if (!transform) return;

  Expect(NearlyEqual(transform->a, 2.0), "recovered a == 2.0");
  Expect(NearlyEqual(transform->b, 0.0), "recovered b == 0.0");
  Expect(NearlyEqual(transform->c, 0.0), "recovered c == 0.0");
  Expect(NearlyEqual(transform->d, 0.0), "recovered d == 0.0");
  Expect(NearlyEqual(transform->e, 2.0), "recovered e == 2.0");
  Expect(NearlyEqual(transform->f, 0.0), "recovered f == 0.0");
}

void TestDegenerateCollinearInputRejected() {
  // All raw y-samples are identical (collinear along the raw x-axis): the
  // normal-equations matrix is singular, so no affine transform can be
  // recovered from this input alone.
  std::array<RawTouchSample, 5> raw = {{{10, 0}, {20, 0}, {30, 0}, {40, 0}, {50, 0}}};
  const auto transform = ComputeAffineTransform(raw, kCalibrationTargets);
  Expect(!transform.has_value(), "collinear raw samples are rejected as degenerate");
}

void TestDegenerateCoincidentInputRejected() {
  std::array<RawTouchSample, 5> raw = {{{5, 5}, {5, 5}, {5, 5}, {5, 5}, {5, 5}}};
  const auto transform = ComputeAffineTransform(raw, kCalibrationTargets);
  Expect(!transform.has_value(), "coincident raw samples are rejected as degenerate");
}

void TestValidationAcceptRejectAtBoundary() {
  const AffineTransform identity{1, 0, 0, 0, 1, 0};
  const ScreenPoint expected{100, 100};

  const auto at_boundary =
      ValidateTap(identity, RawTouchSample{115, 100}, expected, /*max_error_px=*/15.0);
  Expect(at_boundary.accepted, "error exactly at the limit is accepted");
  Expect(NearlyEqual(at_boundary.error_px, 15.0), "boundary error_px reports 15.0");

  const auto past_boundary =
      ValidateTap(identity, RawTouchSample{116, 100}, expected, /*max_error_px=*/15.0);
  Expect(!past_boundary.accepted, "error one pixel past the limit is rejected");

  const auto exact =
      ValidateTap(identity, RawTouchSample{100, 100}, expected, /*max_error_px=*/15.0);
  Expect(exact.accepted, "a zero-error tap is accepted");
  Expect(NearlyEqual(exact.error_px, 0.0), "zero-error tap reports error_px == 0");
}

void TestLandscapeCornerMapping() {
  const auto raw = HalfScaleRawSamples();
  const auto transform = ComputeAffineTransform(raw, kCalibrationTargets);
  Expect(transform.has_value(), "corner-mapping fixture produces a transform");
  if (!transform) return;

  // Targets 0..3 are top-left, top-right, bottom-right, bottom-left in that
  // documented order; each should map back to its own screen corner.
  for (size_t i = 0; i < 4; ++i) {
    const ScreenPoint mapped = ApplyTransform(*transform, raw[i]);
    char what[128];
    std::snprintf(what, sizeof(what), "corner %zu maps back to its target screen point", i);
    Expect(mapped.x == kCalibrationTargets[i].x && mapped.y == kCalibrationTargets[i].y, what);
  }
}

CalibrationRecord MakeValidRecord() {
  CalibrationRecord record;
  record.profile_id = "lcdwiki-esp32-32e-2.8";
  record.orientation = "landscape";
  record.schema_version = firmware::domain::calibration::kCalibrationSchemaVersion;
  record.transform = AffineTransform{2, 0, 0, 0, 2, 0};
  record.checksum = ComputeChecksum(record);
  return record;
}

void TestChecksumRoundTrips() {
  const auto record = MakeValidRecord();
  Expect(IsRecordValid(record, "lcdwiki-esp32-32e-2.8", "landscape",
                       firmware::domain::calibration::kCalibrationSchemaVersion),
         "a freshly checksummed record validates");
}

void TestChecksumDetectsTransformBitCorruption() {
  auto record = MakeValidRecord();
  // Flip one bit inside transform.a's IEEE-754 representation without
  // touching record.checksum, simulating a torn/corrupted NVS blob.
  uint64_t bits;
  std::memcpy(&bits, &record.transform.a, sizeof(bits));
  bits ^= 0x1;
  std::memcpy(&record.transform.a, &bits, sizeof(bits));

  Expect(!IsRecordValid(record, "lcdwiki-esp32-32e-2.8", "landscape",
                        firmware::domain::calibration::kCalibrationSchemaVersion),
         "single-bit transform corruption is detected by checksum mismatch");
}

void TestChecksumDetectsProfileIdCorruption() {
  auto record = MakeValidRecord();
  record.profile_id = "lcdwiki-esp32-32e-2.9";  // one character changed, checksum stale
  Expect(!IsRecordValid(record, "lcdwiki-esp32-32e-2.8", "landscape",
                        firmware::domain::calibration::kCalibrationSchemaVersion),
         "profile_id corruption is detected (identity mismatch and stale checksum)");
}

void TestChecksumDetectsSchemaVersionCorruption() {
  auto record = MakeValidRecord();
  record.schema_version =
      firmware::domain::calibration::kCalibrationSchemaVersion + 1;  // stale checksum
  Expect(!IsRecordValid(record, "lcdwiki-esp32-32e-2.8", "landscape",
                        firmware::domain::calibration::kCalibrationSchemaVersion + 1),
         "schema_version corruption is detected even when the caller expects the new version, "
         "because the stored checksum still covers the old one");
}

void TestIncompatibleOrientationRejected() {
  const auto record = MakeValidRecord();
  Expect(!IsRecordValid(record, "lcdwiki-esp32-32e-2.8", "portrait",
                        firmware::domain::calibration::kCalibrationSchemaVersion),
         "a record for a different orientation is rejected outright");
}

}  // namespace

int main() {
  TestExactRecoveryFromNoiselessPoints();
  TestDegenerateCollinearInputRejected();
  TestDegenerateCoincidentInputRejected();
  TestValidationAcceptRejectAtBoundary();
  TestLandscapeCornerMapping();
  TestChecksumRoundTrips();
  TestChecksumDetectsTransformBitCorruption();
  TestChecksumDetectsProfileIdCorruption();
  TestChecksumDetectsSchemaVersionCorruption();
  TestIncompatibleOrientationRejected();

  if (failures == 0) {
    std::printf("firmware_domain_calibration_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_calibration_tests: %d failure(s)\n", failures);
  return 1;
}
