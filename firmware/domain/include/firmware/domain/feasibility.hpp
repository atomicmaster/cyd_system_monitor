// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace firmware::domain::feasibility {

// A bounded, complete Host snapshot load on the pre-protocol UART link.
// UART framing is 8N1, so every message byte occupies ten wire bits.
struct SerialLoad {
  uint32_t snapshot_bytes = 0;
  uint32_t snapshots_per_second = 0;
  uint32_t uart_baud = 0;

  uint32_t required_bits_per_second = 0;
  int32_t headroom_bits_per_second = 0;
  bool has_headroom = false;

  static SerialLoad Assess(SerialLoad load);
};

// Per-switch receiver evidence. Requested time is intent only: only a
// measured available interval is eligible as verified receive time.
struct CoverageMeasurement {
  uint32_t requested_window_ms = 0;
  uint32_t receiver_available_ms = 0;
  uint32_t uncertain_ms = 0;
  uint32_t packet_processing_loss = 0;

  uint32_t verified_receive_ms() const;
  uint32_t coverage_gap_ms() const;
  bool requested_window_is_verified() const;
};

// The provisional internal-flash capacity calculation. Counts and maximum
// record sizes are inputs because M1a establishes safe values rather than
// treating the initial 32/128/512 targets as promises.
struct ResourceBudget {
  uint32_t flash_partition_bytes = 0;
  uint32_t firmware_image_bytes = 0;
  uint32_t settings_bytes = 0;
  uint32_t journal_reclamation_bytes = 0;
  uint32_t operational_alert_reservation_bytes = 0;
  uint32_t capacity_pressure_alert_reservation_bytes = 0;
  uint32_t active_alert_count = 0;
  uint32_t active_alert_max_bytes = 0;
  uint32_t ended_alert_count = 0;
  uint32_t ended_alert_max_bytes = 0;
  uint32_t baseline_count = 0;
  uint32_t baseline_max_bytes = 0;

  uint64_t retained_record_bytes() const;
  int64_t available_headroom_bytes() const;
  bool fits() const;
};

}  // namespace firmware::domain::feasibility
