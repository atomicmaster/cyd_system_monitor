// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/host_state.hpp"

namespace firmware::domain {
namespace {
// The time a sample was taken, from when it was received and the age the Host
// reported. Signed: a sample older than the device's uptime predates the
// device clock, and clamping it to 0 would make it look fresh.
int64_t SourceTime(uint64_t now_ms, uint32_t age_ms) {
  return static_cast<int64_t>(now_ms) - static_cast<int64_t>(age_ms);
}
// Stale once `stale_after_ms` have passed since the (signed) source time.
protocol::Freshness FreshnessAt(uint64_t now_ms, int64_t source_time_ms, uint64_t stale_after_ms) {
  return static_cast<int64_t>(now_ms) - source_time_ms >= static_cast<int64_t>(stale_after_ms)
             ? protocol::Freshness::kStale
             : protocol::Freshness::kFresh;
}
void DeriveStorageFreshness(StorageMetricState* storage, uint64_t now_ms) {
  storage->io_freshness = FreshnessAt(now_ms, storage->io_received_at_ms, kStorageIoStaleMs);
  storage->capacity_freshness =
      FreshnessAt(now_ms, storage->capacity_source_time_ms, kStorageCapacityStaleMs);
}
void DeriveBatteryFreshness(BatteryMetricState* battery, uint64_t now_ms) {
  battery->charge_freshness =
      FreshnessAt(now_ms, battery->charge_source_time_ms, kBatteryChargeStaleMs);
  battery->condition_freshness =
      FreshnessAt(now_ms, battery->condition_source_time_ms, kBatteryConditionStaleMs);
}
void DeriveAgentsFreshness(AgentsMetricState* agents, uint64_t now_ms) {
  agents->freshness = FreshnessAt(now_ms, agents->source_time_ms, kAgentsStaleMs);
}
void DeriveNetworkFreshness(NetworkMetricState* network, uint64_t now_ms) {
  network->traffic_freshness =
      FreshnessAt(now_ms, network->traffic_source_time_ms, kNetworkTrafficStaleMs);
  network->context_freshness =
      FreshnessAt(now_ms, network->context_source_time_ms, kNetworkContextStaleMs);
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
  const protocol::SnapshotGroups& groups = decoded.groups;
  if (groups.ram) {
    const auto& g = *groups.ram;
    state_.ram = {.has_data = true,
                  .availability = g.availability,
                  .freshness = g.freshness,
                  .used_bytes = g.used_bytes,
                  .total_bytes = g.total_bytes,
                  .source_time_ms = now_ms >= g.source_age_ms ? now_ms - g.source_age_ms : 0,
                  .received_at_ms = now_ms};
  }
  if (groups.storage) {
    const auto& g = *groups.storage;
    state_.storage = {.has_data = true,
                      .capacity_availability = g.capacity_availability,
                      .total_bytes = g.total_bytes,
                      .available_bytes = g.available_bytes,
                      .capacity_source_time_ms = SourceTime(now_ms, g.capacity_age_ms),
                      .io_availability = g.io_availability,
                      .read_bytes_per_s = g.read_bytes_per_s,
                      .write_bytes_per_s = g.write_bytes_per_s,
                      // The I/O sample is at most io_age_ms old when received.
                      .io_received_at_ms = SourceTime(now_ms, g.io_age_ms)};
    DeriveStorageFreshness(&state_.storage, now_ms);
  }
  if (groups.battery) {
    const auto& g = *groups.battery;
    state_.battery = {.has_data = true,
                      .charge_availability = g.charge_availability,
                      .charge_per_mille = g.charge_per_mille,
                      .state = g.state,
                      .time_remaining_min = g.time_remaining_min,
                      .charge_source_time_ms = SourceTime(now_ms, g.charge_age_ms),
                      .condition_availability = g.condition_availability,
                      .max_capacity_per_mille = g.max_capacity_per_mille,
                      .condition = g.condition,
                      .cycle_count = g.cycle_count,
                      .condition_source_time_ms = SourceTime(now_ms, g.condition_age_ms)};
    DeriveBatteryFreshness(&state_.battery, now_ms);
  }
  if (groups.agents) {
    const auto& g = *groups.agents;
    state_.agents = {.has_data = true,
                     .availability = g.availability,
                     .state = g.state,
                     .source_time_ms = SourceTime(now_ms, g.source_age_ms)};
    DeriveAgentsFreshness(&state_.agents, now_ms);
  }
  if (groups.network) {
    const auto& g = *groups.network;
    state_.network = {.has_data = true,
                      .traffic_availability = g.traffic_availability,
                      .route_kind = g.route_kind,
                      .via_tunnel = g.via_tunnel,
                      .rx_bytes_per_s = g.rx_bytes_per_s,
                      .tx_bytes_per_s = g.tx_bytes_per_s,
                      .traffic_source_time_ms = SourceTime(now_ms, g.traffic_age_ms),
                      .context_availability = g.context_availability,
                      .coverage = g.coverage,
                      .channel = g.channel,
                      .station = g.station,
                      .ssid = g.ssid,
                      .bssid = g.bssid,
                      .context_source_time_ms = SourceTime(now_ms, g.context_age_ms),
                      .links = g.links};
    DeriveNetworkFreshness(&state_.network, now_ms);
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
  if (state_.battery.has_data) DeriveBatteryFreshness(&state_.battery, now_ms);
  if (state_.agents.has_data) DeriveAgentsFreshness(&state_.agents, now_ms);
  if (state_.network.has_data) DeriveNetworkFreshness(&state_.network, now_ms);
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
