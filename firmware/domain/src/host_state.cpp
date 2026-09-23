// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/host_state.hpp"

namespace firmware::domain {
namespace {
void DeriveStorageFreshness(StorageMetricState* storage, uint64_t now_ms) {
  storage->io_freshness = now_ms - storage->io_received_at_ms >= kStorageIoStaleMs
                              ? protocol::Freshness::kStale
                              : protocol::Freshness::kFresh;
  storage->capacity_freshness = now_ms - storage->capacity_source_time_ms >= kStorageCapacityStaleMs
                                    ? protocol::Freshness::kStale
                                    : protocol::Freshness::kFresh;
}
}  // namespace
void HostStateReducer::ApplySnapshot(const protocol::Decoded& decoded, uint64_t now_ms,
                                     uint64_t resolved_source_time_ms) {
  if (!decoded.has_cpu) return;
  state_.connection = ConnectionState::kLive;
  state_.cpu.availability = decoded.cpu.availability;
  state_.cpu.freshness = decoded.cpu.freshness;
  state_.cpu.value_per_mille = decoded.cpu.utilization_per_mille;
  state_.cpu.source_time_ms = resolved_source_time_ms;
  state_.cpu.received_at_ms = now_ms;
  state_.host_last_seen_ms = now_ms;
  state_.has_seen_host = true;
  state_.configuration_revision = decoded.cpu.configuration_revision;
  if (decoded.has_ram) {
    state_.ram = {.has_data = true,
                  .availability = decoded.ram.availability,
                  .freshness = decoded.ram.freshness,
                  .used_bytes = decoded.ram.used_bytes,
                  .total_bytes = decoded.ram.total_bytes,
                  .source_time_ms =
                      now_ms >= decoded.ram.source_age_ms ? now_ms - decoded.ram.source_age_ms : 0,
                  .received_at_ms = now_ms};
  }
  if (decoded.has_storage) {
    const auto& g = decoded.storage;
    state_.storage = {
        .has_data = true,
        .capacity_availability = g.capacity_availability,
        .total_bytes = g.total_bytes,
        .available_bytes = g.available_bytes,
        .capacity_source_time_ms = now_ms >= g.capacity_age_ms ? now_ms - g.capacity_age_ms : 0,
        .io_availability = g.io_availability,
        .read_bytes_per_s = g.read_bytes_per_s,
        .write_bytes_per_s = g.write_bytes_per_s,
        // The I/O sample is at most io_age_ms old when received.
        .io_received_at_ms = now_ms >= g.io_age_ms ? now_ms - g.io_age_ms : 0};
    DeriveStorageFreshness(&state_.storage, now_ms);
  }
}
void HostStateReducer::Advance(uint64_t now_ms) {
  if (state_.connection == ConnectionState::kIncompatible || !state_.has_seen_host) return;
  const uint64_t age = now_ms - state_.host_last_seen_ms;
  if (age >= 15000)
    state_.connection = ConnectionState::kDisconnected;
  else if (age >= 3000) {
    state_.connection = ConnectionState::kStale;
    state_.cpu.freshness = protocol::Freshness::kStale;
  }
  if (state_.ram.has_data && now_ms - state_.ram.received_at_ms >= 3000)
    state_.ram.freshness = protocol::Freshness::kStale;
  if (state_.storage.has_data) DeriveStorageFreshness(&state_.storage, now_ms);
}
void HostStateReducer::MarkIncompatible() { state_.connection = ConnectionState::kIncompatible; }
void HostStateReducer::CorrectTime() { state_.time_available = true; }
void HostStateReducer::SetUtcAnchor(uint64_t utc_ms, uint64_t monotonic_ms,
                                    std::optional<int16_t> local_offset_minutes) {
  state_.time_available = true;
  // The Host repeats its offset with every UTC value, so a daylight-saving
  // change or a timezone move follows it; an absent offset reverts to UTC.
  state_.has_utc_offset = local_offset_minutes.has_value();
  state_.utc_offset_minutes = local_offset_minutes.value_or(0);
  state_.utc_anchor_ms = utc_ms;
  state_.utc_anchor_monotonic_ms = monotonic_ms;
}
void Acknowledge(AlertEpisode* episode) {
  if (episode) episode->acknowledged = true;
}
void EndObservedQuiet(AlertEpisode* episode, uint64_t order) {
  if (episode && episode->state == EpisodeState::kActive) {
    episode->state = EpisodeState::kEnded;
    episode->last_evidence_order = order;
  }
}
void InterruptForRecovery(AlertEpisode* episode) {
  if (episode && episode->state == EpisodeState::kActive)
    episode->state = EpisodeState::kInterrupted;
}
}  // namespace firmware::domain
