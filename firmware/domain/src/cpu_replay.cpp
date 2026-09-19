// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/cpu_replay.hpp"

namespace firmware::domain {
namespace {

using Session = std::array<uint8_t, protocol::kSessionIdBytes>;

Session SessionId(uint8_t first) {
  Session id{};
  for (uint8_t i = 0; i < id.size(); ++i) id[i] = i;
  id[0] = first;
  return id;
}

struct Snapshot {
  uint32_t sequence = 1;
  protocol::Availability availability = protocol::Availability::kAvailable;
  uint16_t per_mille = 0;
  uint32_t source_age_ms = 0;
  uint8_t session = 1;
  std::optional<uint64_t> utc_ms;
  uint8_t major = 1;
};

std::vector<uint8_t> Frame(const Snapshot& s) {
  protocol::Envelope envelope;
  envelope.version.major = s.major;
  envelope.type = protocol::MessageType::kSnapshot;
  envelope.session = SessionId(s.session);
  envelope.sequence = s.sequence;
  envelope.utc_ms = s.utc_ms;
  std::vector<uint8_t> frame;
  protocol::EncodeCpuSnapshot(
      envelope, {s.availability, s.per_mille, s.source_age_ms, protocol::Freshness::kFresh, 3},
      &frame);
  return frame;
}

constexpr uint64_t kDay = 86400000ULL * 20000;

}  // namespace

// Checkpoints sit 100 ms or more away from every 3 s / 15 s boundary so a
// 20 ms board loop and a 100 ms simulator step observe the same states.
CpuReplayScript MakeCpuReplayScript() {
  CpuReplayScript script;
  auto frame = [&](uint64_t at, const Snapshot& s) {
    script.events.push_back({at, Frame(s), std::nullopt});
  };
  auto checkpoint = [&](uint64_t at, const char* label) {
    script.checkpoints.push_back({at, label});
  };
  script.events.push_back({0, {}, SessionId(1)});
  checkpoint(500, "cold-boot");
  frame(1000, {.sequence = 1, .availability = protocol::Availability::kTemporarilyUnavailable});
  checkpoint(1500, "first-unavailable-rate");
  frame(2000, {.sequence = 2, .availability = protocol::Availability::kUnsupported});
  checkpoint(2500, "state-unsupported");
  frame(3000, {.sequence = 3, .availability = protocol::Availability::kAbsent});
  checkpoint(3500, "state-absent");
  frame(4000, {.sequence = 4, .availability = protocol::Availability::kError});
  checkpoint(4500, "state-error");
  frame(5000, {.sequence = 5,
               .per_mille = 623,
               .source_age_ms = 100,
               .utc_ms = kDay + (13 * 3600 + 7 * 60) * 1000ULL});
  checkpoint(5500, "valid-live");
  checkpoint(7900, "live-under-3s");
  checkpoint(8100, "stale-after-3s");
  checkpoint(19900, "stale-under-15s");
  checkpoint(20100, "disconnected-after-15s");
  frame(21000, {.sequence = 6, .per_mille = 410, .source_age_ms = 2500});
  checkpoint(21500, "recovered-old-source");
  frame(22000, {.sequence = 7, .per_mille = 400, .utc_ms = kDay + (2 * 3600 + 30 * 60) * 1000ULL});
  checkpoint(22500, "time-correction-backwards");
  frame(23000, {.sequence = 7, .per_mille = 999});
  checkpoint(23500, "reject-stale-sequence");
  frame(24000, {.sequence = 12, .per_mille = 999, .session = 9});
  checkpoint(24500, "reject-wrong-session");
  auto damaged = Frame({.sequence = 8, .per_mille = 999});
  damaged[12] ^= 0x10;
  script.events.push_back({25000, damaged, std::nullopt});
  checkpoint(25500, "reject-bad-crc");
  frame(26000, {.sequence = 8, .per_mille = 700});
  checkpoint(26500, "recovered-after-reject");
  frame(27000, {.sequence = 9, .major = 2});
  checkpoint(27500, "incompatible-major");
  script.events.push_back({28000, {}, SessionId(2)});
  frame(28000, {.sequence = 1, .per_mille = 250, .session = 2});
  checkpoint(28500, "reconnect-new-session");
  script.duration_ms = 29000;
  return script;
}

std::string FormatCheckpoint(const char* label, const CpuView& view) {
  return std::string(label) + " | " + view.connection + " | " + view.clock + " | " + view.value +
         " | " + view.note + " | dimmed=" + (view.dimmed ? "1" : "0");
}

void CpuReplay::Start() {
  start_ms_ = clock_.NowMs();
  next_event_ = 0;
  next_checkpoint_ = 0;
  started_ = true;
}

const char* CpuReplay::Poll() {
  if (!started_) return nullptr;
  const uint64_t elapsed = elapsed_ms();
  // An event and its checkpoint share a batch; only events at or before the
  // next checkpoint are applied, so a late poll cannot skip a checkpoint's state.
  const uint64_t horizon = next_checkpoint_ < script_.checkpoints.size()
                               ? script_.checkpoints[next_checkpoint_].at_ms
                               : elapsed;
  while (next_event_ < script_.events.size() &&
         script_.events[next_event_].at_ms <= (elapsed < horizon ? elapsed : horizon)) {
    const ReplayEvent& event = script_.events[next_event_++];
    if (event.start_session) feed_.StartSession(*event.start_session);
    if (!event.bytes.empty()) feed_.Push(event.bytes);
  }
  feed_.Tick();
  if (next_checkpoint_ < script_.checkpoints.size() &&
      script_.checkpoints[next_checkpoint_].at_ms <= elapsed) {
    return script_.checkpoints[next_checkpoint_++].label;
  }
  return nullptr;
}

}  // namespace firmware::domain
