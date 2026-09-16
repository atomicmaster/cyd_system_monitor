// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/feasibility.hpp"

#include <algorithm>
#include <limits>

namespace firmware::domain::feasibility {

namespace {

uint64_t SaturatingMultiply(uint64_t left, uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
    return std::numeric_limits<uint64_t>::max();
  }
  return left * right;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return std::numeric_limits<uint64_t>::max();
  }
  return left + right;
}

}  // namespace

SerialLoad SerialLoad::Assess(SerialLoad load) {
  const uint64_t required =
      SaturatingMultiply(SaturatingMultiply(static_cast<uint64_t>(load.snapshot_bytes), 10U),
                         static_cast<uint64_t>(load.snapshots_per_second));
  load.required_bits_per_second = required > std::numeric_limits<uint32_t>::max()
                                      ? std::numeric_limits<uint32_t>::max()
                                      : static_cast<uint32_t>(required);
  const int64_t headroom =
      required > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
          ? std::numeric_limits<int64_t>::min()
          : static_cast<int64_t>(load.uart_baud) - static_cast<int64_t>(required);
  load.headroom_bits_per_second = std::clamp<int64_t>(headroom, std::numeric_limits<int32_t>::min(),
                                                      std::numeric_limits<int32_t>::max());
  load.has_headroom = headroom > 0;
  return load;
}

uint32_t CoverageMeasurement::verified_receive_ms() const {
  const uint32_t certain_receive_ms =
      receiver_available_ms > uncertain_ms ? receiver_available_ms - uncertain_ms : 0;
  return std::min(requested_window_ms, certain_receive_ms);
}

uint32_t CoverageMeasurement::coverage_gap_ms() const {
  // Uncertain time cannot be claimed as verified receive time. It remains a
  // separately reported measurement field, and also contributes to the gap.
  return requested_window_ms - verified_receive_ms();
}

bool CoverageMeasurement::requested_window_is_verified() const {
  return verified_receive_ms() == requested_window_ms;
}

uint64_t ResourceBudget::retained_record_bytes() const {
  return SaturatingAdd(SaturatingAdd(SaturatingMultiply(active_alert_count, active_alert_max_bytes),
                                     SaturatingMultiply(ended_alert_count, ended_alert_max_bytes)),
                       SaturatingMultiply(baseline_count, baseline_max_bytes));
}

int64_t ResourceBudget::available_headroom_bytes() const {
  if (retained_record_bytes() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(flash_partition_bytes) - static_cast<int64_t>(firmware_image_bytes) -
         static_cast<int64_t>(settings_bytes) - static_cast<int64_t>(journal_reclamation_bytes) -
         static_cast<int64_t>(operational_alert_reservation_bytes) -
         static_cast<int64_t>(capacity_pressure_alert_reservation_bytes) -
         static_cast<int64_t>(retained_record_bytes());
}

bool ResourceBudget::fits() const { return available_headroom_bytes() >= 0; }

}  // namespace firmware::domain::feasibility
