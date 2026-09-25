// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "protocol.hpp"

namespace firmware::domain {

namespace protocol = cyd::protocol;

// The UI consumes this state rather than interpreting transport arrival time.
// Availability, freshness, and adapter enablement deliberately remain separate.
enum class ConnectionState { kNegotiating, kLive, kStale, kDisconnected, kIncompatible };
struct HostMetricState {
  protocol::Availability availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness freshness = protocol::Freshness::kFresh;
  bool adapter_enabled = true;
  uint16_t value_per_mille = 0;
  uint64_t source_time_ms = 0;
  uint64_t received_at_ms = 0;
};
// Physical RAM Occupied (C02's first metric group). Absent until a snapshot
// carries the group; freshness follows the same 3 s rule as CPU.
struct RamMetricState {
  bool has_data = false;
  protocol::Availability availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness freshness = protocol::Freshness::kFresh;
  uint64_t used_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t source_time_ms = 0;
  uint64_t received_at_ms = 0;
};
// Root storage (TB04): capacity and disk I/O, each with its own availability
// and source time. The wire carries no freshness; the device derives it here:
// I/O is stale 3 s after it was received, capacity 90 s after its *source*
// sample (received time minus the reported age), so a new snapshot that
// re-carries an old capacity sample does not make it fresh.
constexpr uint64_t kStorageIoStaleMs = 3000;
constexpr uint64_t kStorageCapacityStaleMs = 90000;
struct StorageMetricState {
  bool has_data = false;
  protocol::Availability capacity_availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness capacity_freshness = protocol::Freshness::kFresh;
  uint64_t total_bytes = 0;
  uint64_t available_bytes = 0;
  int64_t capacity_source_time_ms = 0;  // signed: a sample older than boot is negative
  protocol::Availability io_availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness io_freshness = protocol::Freshness::kFresh;
  uint64_t read_bytes_per_s = 0;
  uint64_t write_bytes_per_s = 0;
  int64_t io_received_at_ms = 0;  // signed for the same reason
};
// Battery (TB06): charge/state and condition, each with its own availability
// and source time. The wire carries no freshness; the device derives it from
// the *source* sample time (received time minus the reported age): charge is
// stale 90 s after it was sampled (30 s cadence x 3), condition 3 h after
// (1 h cadence x 3). A snapshot that re-carries an old sample stays stale.
constexpr uint64_t kBatteryChargeStaleMs = 90'000;
constexpr uint64_t kBatteryConditionStaleMs = 3ULL * 3600 * 1000;
struct BatteryMetricState {
  bool has_data = false;
  protocol::Availability charge_availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness charge_freshness = protocol::Freshness::kFresh;
  uint16_t charge_per_mille = 0;
  protocol::BatteryState state = protocol::BatteryState::kUnknown;
  uint16_t time_remaining_min = protocol::kBatteryUnknownU16;
  // Signed: a 3 h old sample received soon after boot happened before the
  // device's clock started, and clamping it to 0 would make it look fresh.
  int64_t charge_source_time_ms = 0;
  protocol::Availability condition_availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness condition_freshness = protocol::Freshness::kFresh;
  uint16_t max_capacity_per_mille = 0;
  protocol::BatteryCondition condition = protocol::BatteryCondition::kUnknown;
  uint16_t cycle_count = protocol::kBatteryUnknownU16;
  int64_t condition_source_time_ms = 0;
};
// Agents (TB07): same-user process presence for [codex, claude, grok]. The
// wire carries no freshness; the device derives it from the *source* sample
// time (received time minus the reported age): stale 15 s after the sample
// (5 s cadence x 3). A re-sent old sample stays stale.
constexpr uint64_t kAgentsStaleMs = 15'000;
struct AgentsMetricState {
  bool has_data = false;
  protocol::Availability availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness freshness = protocol::Freshness::kFresh;
  // In the wire's slot order: codex, claude, grok.
  std::array<protocol::AgentState, protocol::kAgentSlots> state{};
  // Signed: a sample older than device uptime predates the device clock.
  int64_t source_time_ms = 0;
};
// Network (TB05): primary-route traffic and Host WiFi context, each half with
// its own availability and source time. The wire carries no freshness; the
// device derives it from the *source* sample time (received time minus the
// reported age), so a re-sent old sample stays stale. Traffic is stale 3 s
// after its sample (the 1 s rate cadence x 3, the same rule as CPU). WiFi
// context is stale 15 s after its sample: the provider sends on change and at
// least once a second, so 15 s is 15 missed heartbeats, and the Host itself
// downgrades a silent provider's context to unavailable at the same 15 s
// (docs/wifi-context-provider.md). Host Station Identity is read by the Host
// itself every second and travels with the traffic half's age.
constexpr uint64_t kNetworkTrafficStaleMs = 3'000;
constexpr uint64_t kNetworkContextStaleMs = 15'000;
struct NetworkMetricState {
  bool has_data = false;
  protocol::Availability traffic_availability = protocol::Availability::kTemporarilyUnavailable;
  protocol::Freshness traffic_freshness = protocol::Freshness::kFresh;
  protocol::RouteKind route_kind = protocol::RouteKind::kUnknown;
  bool via_tunnel = false;
  uint64_t rx_bytes_per_s = 0;
  uint64_t tx_bytes_per_s = 0;
  // Signed: a sample older than device uptime predates the device clock.
  int64_t traffic_source_time_ms = 0;
  protocol::AssociationContext context_availability =
      protocol::AssociationContext::kPermissionNotRequested;
  protocol::Freshness context_freshness = protocol::Freshness::kFresh;
  protocol::WifiCoverage coverage = protocol::WifiCoverage::kUnknown;
  uint8_t channel = 0;
  std::optional<std::array<uint8_t, protocol::kMacBytes>> station;
  std::vector<uint8_t> ssid;
  std::optional<std::array<uint8_t, protocol::kMacBytes>> bssid;
  int64_t context_source_time_ms = 0;
};
struct HostState {
  ConnectionState connection = ConnectionState::kNegotiating;
  HostMetricState cpu{};
  RamMetricState ram{};
  StorageMetricState storage{};
  BatteryMetricState battery{};
  AgentsMetricState agents{};
  NetworkMetricState network{};
  uint64_t host_last_seen_ms = 0;
  bool has_seen_host = false;  // false on cold boot: last-seen is unknown, not zero
  bool time_available = false;
  // UTC anchor: Host-supplied UTC at a monotonic instant. Display-only; it is
  // never an ordering key and is replaced (not applied retroactively) on a
  // Time Discontinuity.
  uint64_t utc_anchor_ms = 0;
  uint64_t utc_anchor_monotonic_ms = 0;
  // Host-supplied local offset (minutes east of UTC); absent from older Hosts,
  // in which case the clock shows UTC.
  bool has_utc_offset = false;
  int16_t utc_offset_minutes = 0;
  uint32_t boot_identity = 0;
  uint32_t configuration_revision = 0;

  // The link to the Host is lost (stale, disconnected, or incompatible):
  // every card retains its last reading with "Last" and dims it.
  bool LinkLost() const {
    return connection == ConnectionState::kStale || connection == ConnectionState::kIncompatible ||
           connection == ConnectionState::kDisconnected;
  }
};

class HostStateReducer {
 public:
  explicit HostStateReducer(uint32_t boot_identity) : state_{.boot_identity = boot_identity} {}
  void ApplySnapshot(const protocol::Decoded& decoded, uint64_t now_ms,
                     uint64_t resolved_source_time_ms);
  void Advance(uint64_t now_ms);
  void MarkIncompatible();
  void CorrectTime();  // Existing evidence is intentionally not rewritten.
  void SetUtcAnchor(uint64_t utc_ms, uint64_t monotonic_ms,
                    std::optional<int16_t> local_offset_minutes = std::nullopt);
  const HostState& state() const { return state_; }

 private:
  HostState state_;
};

// Minimal shared episode lifecycle: later radio rules supply eligibility and
// evidence; this model preserves acknowledgment and interruptions correctly.
enum class EpisodeState { kActive, kEnded, kInterrupted };
struct AlertEpisode {
  EpisodeState state = EpisodeState::kActive;
  bool acknowledged = false;
  uint64_t first_evidence_order = 0;
  uint64_t last_evidence_order = 0;
};
void Acknowledge(AlertEpisode* episode);
void EndObservedQuiet(AlertEpisode* episode, uint64_t evidence_order);
void InterruptForRecovery(AlertEpisode* episode);

}  // namespace firmware::domain
