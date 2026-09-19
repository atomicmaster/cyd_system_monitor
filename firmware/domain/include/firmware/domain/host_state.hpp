// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

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
struct HostState {
  ConnectionState connection = ConnectionState::kNegotiating;
  HostMetricState cpu{};
  uint64_t host_last_seen_ms = 0;
  bool has_seen_host = false;  // false on cold boot: last-seen is unknown, not zero
  bool time_available = false;
  // UTC anchor: Host-supplied UTC at a monotonic instant. Display-only; it is
  // never an ordering key and is replaced (not applied retroactively) on a
  // Time Discontinuity.
  uint64_t utc_anchor_ms = 0;
  uint64_t utc_anchor_monotonic_ms = 0;
  uint32_t boot_identity = 0;
  uint32_t configuration_revision = 0;
};

class HostStateReducer {
 public:
  explicit HostStateReducer(uint32_t boot_identity) : state_{.boot_identity = boot_identity} {}
  void ApplySnapshot(const protocol::Decoded& decoded, uint64_t now_ms,
                     uint64_t resolved_source_time_ms);
  void Advance(uint64_t now_ms);
  void MarkIncompatible();
  void CorrectTime();  // Existing evidence is intentionally not rewritten.
  void SetUtcAnchor(uint64_t utc_ms, uint64_t monotonic_ms);
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
