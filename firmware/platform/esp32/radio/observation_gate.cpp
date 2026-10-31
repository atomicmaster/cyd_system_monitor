// SPDX-License-Identifier: Apache-2.0
#include "observation_gate.hpp"

#include <algorithm>

namespace firmware::platform::esp32::radio {

namespace {

using domain::EventProtocol;
using domain::ObservationEvent;
using domain::Radio;

size_t Index(EventProtocol protocol) { return protocol == EventProtocol::kWifi ? 0 : 1; }
Radio RadioOf(EventProtocol protocol) {
  return protocol == EventProtocol::kWifi ? Radio::kWifi : Radio::kBle;
}

}  // namespace

void ObservationGate::BeginWindow(Radio radio, uint32_t window_id, uint32_t generation,
                                  uint64_t start_us, uint64_t deadline_us) {
  portENTER_CRITICAL(&mux_);
  window_ = {true, radio, window_id, generation, start_us, deadline_us};
  portEXIT_CRITICAL(&mux_);
}

void ObservationGate::EndWindow() {
  portENTER_CRITICAL(&mux_);
  window_.open = false;
  portEXIT_CRITICAL(&mux_);
}

bool ObservationGate::Offer(ObservationEvent event) {
  const size_t index = Index(event.protocol);
  bool accepted = false;
  portENTER_CRITICAL(&mux_);
  if (window_.open && event.monotonic_us >= window_.start_us &&
      event.monotonic_us <= window_.deadline_us) {
    if (window_.radio == RadioOf(event.protocol)) {
      event.window_id = window_.id;
      event.config_generation = window_.generation;
      // Nested spinlock: the ring's own, always taken inside this one.
      if (event.protocol == EventProtocol::kWifi) {
        wifi_ring_.Push(event);
      } else {
        ble_ring_.Push(event);
      }
      accepted = true;
    } else {
      ++outside_window_[index];
      ++wrong_radio_[index];
    }
  } else {
    ++outside_window_[index];
  }
  portEXIT_CRITICAL(&mux_);
  return accepted;
}

size_t ObservationGate::Drain() {
  ObservationEvent batch[kDrainBatch];
  // Enough for both full rings plus the events that arrive while draining.
  constexpr size_t kMaxBatches = (kWifiRingEvents + kBleRingEvents) / kDrainBatch + 4;
  size_t total = 0;
  for (size_t round = 0; round < kMaxBatches; ++round) {
    size_t moved = 0;
    size_t count = wifi_ring_.PopBatch(batch, kDrainBatch);
    // Counting sink until TB10/TB11 parsers exist: tally per protocol.
    drained_[0].fetch_add(static_cast<uint32_t>(count), std::memory_order_relaxed);
    moved += count;
    count = ble_ring_.PopBatch(batch, kDrainBatch);
    drained_[1].fetch_add(static_cast<uint32_t>(count), std::memory_order_relaxed);
    moved += count;
    if (moved == 0) break;
    total += moved;
  }
  return total;
}

uint32_t ObservationGate::TakeDrops(uint32_t window_id) {
  return wifi_ring_.TakeDrops(window_id) + ble_ring_.TakeDrops(window_id);
}

uint32_t ObservationGate::TakeUnattributed() {
  const uint32_t taken = wifi_ring_.TakeUnattributed() + ble_ring_.TakeUnattributed();
  unattributed_drops_.fetch_add(taken, std::memory_order_relaxed);
  return taken;
}

ObservationGate::Counts ObservationGate::counts() const {
  Counts counts;
  portENTER_CRITICAL(&mux_);
  for (size_t i = 0; i < 2; ++i) {
    counts.outside_window[i] = outside_window_[i];
    counts.wrong_radio[i] = wrong_radio_[i];
  }
  portEXIT_CRITICAL(&mux_);
  for (size_t i = 0; i < 2; ++i) counts.drained[i] = drained_[i].load(std::memory_order_relaxed);
  counts.unattributed_drops = unattributed_drops_.load(std::memory_order_relaxed);
  return counts;
}

uint64_t ObservationGate::ring_drops_total() const {
  return wifi_ring_.dropped() + ble_ring_.dropped();
}

}  // namespace firmware::platform::esp32::radio
