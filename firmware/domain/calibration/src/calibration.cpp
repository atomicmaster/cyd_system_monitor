// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/calibration/calibration.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace firmware::domain::calibration {

namespace {

// Solves the shared 3x3 normal-equations system
//   [ Sxx Sxy Sx ] [a]   [SxX]
//   [ Sxy Syy Sy ] [b] = [SyX]
//   [ Sx  Sy  N  ] [c]   [SX ]
// for one axis at a time (X targets first, then Y targets), sharing the
// same matrix since it only depends on the raw points. Returns false if
// the matrix is (near-)singular, which happens for degenerate raw input
// such as collinear or coincident points.
bool SolveAxis(double sxx, double sxy, double sx, double syy, double sy, double n, double b0,
               double b1, double b2, double& out0, double& out1, double& out2) {
  // 3x3 determinant.
  const double det =
      sxx * (syy * n - sy * sy) - sxy * (sxy * n - sy * sx) + sx * (sxy * sy - syy * sx);
  constexpr double kMinDeterminant = 1e-6;
  if (std::fabs(det) < kMinDeterminant) {
    return false;
  }

  // Cramer's rule: replace each column with the right-hand side in turn.
  const double det0 =
      b0 * (syy * n - sy * sy) - sxy * (b1 * n - sy * b2) + sx * (b1 * sy - syy * b2);
  const double det1 =
      sxx * (b1 * n - b2 * sy) - b0 * (sxy * n - sy * sx) + sx * (sxy * b2 - b1 * sx);
  const double det2 =
      sxx * (syy * b2 - b1 * sy) - sxy * (sxy * b2 - b1 * sx) + b0 * (sxy * sy - syy * sx);

  out0 = det0 / det;
  out1 = det1 / det;
  out2 = det2 / det;
  return true;
}

void AppendBytes(std::vector<uint8_t>& buffer, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  buffer.insert(buffer.end(), bytes, bytes + size);
}

// Bit-by-bit CRC32 (polynomial 0xEDB88320, the standard reflected IEEE
// 802.3 polynomial), avoiding an external dependency for such a small,
// non-performance-critical checksum.
uint32_t Crc32(const std::vector<uint8_t>& data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

}  // namespace

std::optional<AffineTransform> ComputeAffineTransform(const std::array<RawTouchSample, 5>& raw,
                                                      const std::array<ScreenPoint, 5>& targets) {
  double sxx = 0, sxy = 0, sx = 0, syy = 0, sy = 0;
  double sxX = 0, syX = 0, sX = 0;
  double sxY = 0, syY = 0, sY = 0;
  const double n = static_cast<double>(raw.size());

  for (size_t i = 0; i < raw.size(); ++i) {
    const double x = static_cast<double>(raw[i].raw_x);
    const double y = static_cast<double>(raw[i].raw_y);
    const double X = static_cast<double>(targets[i].x);
    const double Y = static_cast<double>(targets[i].y);

    sxx += x * x;
    sxy += x * y;
    sx += x;
    syy += y * y;
    sy += y;

    sxX += x * X;
    syX += y * X;
    sX += X;

    sxY += x * Y;
    syY += y * Y;
    sY += Y;
  }

  AffineTransform transform;
  if (!SolveAxis(sxx, sxy, sx, syy, sy, n, sxX, syX, sX, transform.a, transform.b, transform.c)) {
    return std::nullopt;
  }
  if (!SolveAxis(sxx, sxy, sx, syy, sy, n, sxY, syY, sY, transform.d, transform.e, transform.f)) {
    return std::nullopt;
  }
  return transform;
}

ScreenPoint ApplyTransform(const AffineTransform& transform, RawTouchSample raw) {
  const double x = static_cast<double>(raw.raw_x);
  const double y = static_cast<double>(raw.raw_y);
  return ScreenPoint{
      static_cast<int32_t>(std::lround(transform.a * x + transform.b * y + transform.c)),
      static_cast<int32_t>(std::lround(transform.d * x + transform.e * y + transform.f)),
  };
}

ValidationResult ValidateTap(const AffineTransform& transform, RawTouchSample observed,
                             ScreenPoint expected, double max_error_px) {
  const ScreenPoint mapped = ApplyTransform(transform, observed);
  const double dx = static_cast<double>(mapped.x - expected.x);
  const double dy = static_cast<double>(mapped.y - expected.y);
  const double error_px = std::sqrt(dx * dx + dy * dy);
  return ValidationResult{error_px <= max_error_px, error_px};
}

uint32_t ComputeChecksum(const CalibrationRecord& record) {
  std::vector<uint8_t> buffer;
  AppendBytes(buffer, record.profile_id.data(), record.profile_id.size());
  AppendBytes(buffer, record.orientation.data(), record.orientation.size());
  AppendBytes(buffer, &record.schema_version, sizeof(record.schema_version));
  const double components[6] = {record.transform.a, record.transform.b, record.transform.c,
                                record.transform.d, record.transform.e, record.transform.f};
  for (double component : components) {
    AppendBytes(buffer, &component, sizeof(component));
  }
  return Crc32(buffer);
}

bool IsRecordValid(const CalibrationRecord& record, std::string_view expected_profile_id,
                   std::string_view expected_orientation, uint32_t expected_schema_version) {
  if (record.profile_id != expected_profile_id) return false;
  if (record.orientation != expected_orientation) return false;
  if (record.schema_version != expected_schema_version) return false;
  return ComputeChecksum(record) == record.checksum;
}

}  // namespace firmware::domain::calibration
