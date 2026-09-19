// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <vector>

#include "../expect.hpp"
#include "firmware/domain/host_state.hpp"
#include "protocol.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
int RunProtocolReplay() {
  ScenarioResult result;
  cyd::protocol::Envelope envelope;
  envelope.type = cyd::protocol::MessageType::kSnapshot;
  envelope.sequence = 1;
  for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
  const cyd::protocol::CpuSnapshot cpu{cyd::protocol::Availability::kAvailable, 620, 100,
                                       cyd::protocol::Freshness::kFresh, 3};
  std::vector<uint8_t> frame;
  result.Expect(
      cyd::protocol::EncodeCpuSnapshot(envelope, cpu, &frame) == cyd::protocol::Error::kNone,
      "the complete initial snapshot fits the bounded frame");
  cyd::protocol::Decoded decoded;
  result.Expect(cyd::protocol::Decode(frame, &decoded) == cyd::protocol::Error::kNone,
                "the firmware-side decoder accepts the replay fixture");
  cyd::protocol::SessionReceiver session;
  result.Expect(session.Accept(decoded) == cyd::protocol::Error::kNone,
                "first negotiated-session snapshot is accepted");
  firmware::domain::HostStateReducer state(17);
  state.ApplySnapshot(decoded, 1000, 900);
  state.Advance(4000);
  result.Expect(state.state().connection == firmware::domain::ConnectionState::kStale,
                "three seconds without snapshots makes the retained value stale");
  state.Advance(16000);
  result.Expect(state.state().connection == firmware::domain::ConnectionState::kDisconnected,
                "fifteen seconds without snapshots marks the Host disconnected");
  state.CorrectTime();
  result.Expect(state.state().time_available && state.state().cpu.source_time_ms == 900,
                "time correction never rewrites prior evidence ordering");
  envelope.sequence = 1;
  envelope.session[0] = 99;  // reconnect gives a fresh session identity
  cyd::protocol::EncodeCpuSnapshot(envelope, cpu, &frame);
  cyd::protocol::Decode(frame, &decoded);
  result.Expect(session.Accept(decoded) == cyd::protocol::Error::kWrongSession,
                "old receiver rejects a reconnect until session reset");
  session.Reset(decoded.envelope.session);
  result.Expect(session.Accept(decoded) == cyd::protocol::Error::kNone,
                "reset accepts sequence one from the new negotiated session");
  return result.Finish("simulator protocol-replay");
}
}  // namespace simulator::scenarios
