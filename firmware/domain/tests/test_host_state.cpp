// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <vector>

#include "firmware/domain/host_state.hpp"

int main() {
  cyd::protocol::Envelope e;
  e.type = cyd::protocol::MessageType::kSnapshot;
  e.sequence = 1;
  cyd::protocol::CpuSnapshot cpu{cyd::protocol::Availability::kAvailable, 500, 7000,
                                 cyd::protocol::Freshness::kFresh, 2};
  std::vector<uint8_t> wire;
  cyd::protocol::EncodeCpuSnapshot(e, cpu, &wire);
  cyd::protocol::Decoded d;
  if (cyd::protocol::Decode(wire, &d) != cyd::protocol::Error::kNone) return 1;
  firmware::domain::HostStateReducer state(42);
  state.ApplySnapshot(d, 1000, 900);
  state.Advance(4000);
  if (state.state().connection != firmware::domain::ConnectionState::kStale) return 2;
  state.Advance(16000);
  if (state.state().connection != firmware::domain::ConnectionState::kDisconnected) return 3;
  firmware::domain::AlertEpisode alert{.first_evidence_order = 9};
  firmware::domain::Acknowledge(&alert);
  if (!alert.acknowledged || alert.state != firmware::domain::EpisodeState::kActive) return 4;
  firmware::domain::InterruptForRecovery(&alert);
  if (alert.state != firmware::domain::EpisodeState::kInterrupted) return 5;
  std::puts("firmware_domain_host_state_tests: ok");
  return 0;
}
