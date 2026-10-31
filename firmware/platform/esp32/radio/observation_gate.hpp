// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "firmware/domain/coverage.hpp"
#include "firmware/domain/event_ring.hpp"
#include "firmware/domain/observation_event.hpp"
#include "port_lock.hpp"

namespace firmware::platform::esp32::radio {

// What the receive callbacks and the radio task share: the open-window record
// and one event ring per protocol. A callback offers an event; it is stamped
// with the open window and pushed only if a window of its own radio is open and
// the event's timestamp lies in [start, deadline]. Anything else is counted as
// outside-window and never pushed (RadioDriver window contract).
//
// The window check and the push happen in one critical section, so once
// EndWindow returns no event can enter the ring for that window.
class ObservationGate {
 public:
  static constexpr size_t kWifiRingEvents = 128;
  static constexpr size_t kBleRingEvents = 96;
  // Events moved per PopBatch; bounds the stack used while draining.
  static constexpr size_t kDrainBatch = 8;

  struct Counts {
    uint32_t outside_window[2] = {};  // indexed by EventProtocol
    uint32_t wrong_radio[2] = {};  // subset of outside_window: a window of the other radio was open
    uint32_t drained[2] = {};      // events handed to the (counting) sink
    uint32_t unattributed_drops = 0;  // drops taken without a window of their own
  };

  void BeginWindow(domain::Radio radio, uint32_t window_id, uint32_t generation, uint64_t start_us,
                   uint64_t deadline_us);
  void EndWindow();

  // Callback side. Returns whether the event entered a ring.
  bool Offer(domain::ObservationEvent event);

  // Radio task side: pops every queued event (bounded batches) into the
  // counting sink. Returns the number moved.
  size_t Drain();
  uint32_t TakeDrops(uint32_t window_id);
  uint32_t TakeUnattributed();

  Counts counts() const;
  uint64_t ring_drops_total() const;

 private:
  struct Window {
    bool open = false;
    domain::Radio radio = domain::Radio::kWifi;
    uint32_t id = 0;
    uint32_t generation = 0;
    uint64_t start_us = 0;
    uint64_t deadline_us = 0;
  };

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  Window window_;
  uint32_t outside_window_[2] = {};
  uint32_t wrong_radio_[2] = {};
  domain::EventRing<domain::ObservationEvent, kWifiRingEvents, PortMuxLock> wifi_ring_;
  domain::EventRing<domain::ObservationEvent, kBleRingEvents, PortMuxLock> ble_ring_;
  // Written by the radio task only; read by the console for DEV status.
  std::atomic<uint32_t> drained_[2] = {};
  std::atomic<uint32_t> unattributed_drops_{0};
};

}  // namespace firmware::platform::esp32::radio
