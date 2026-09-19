// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/host_feed.hpp"

namespace firmware::domain {

// The scripted CPU-snapshot timeline replayed by the simulator (`cpu-replay`)
// and by the board (`DEV:CPU_REPLAY`), so a physical display check needs no
// Mac collector. Both run the same bytes through the same HostFeed and log the
// same checkpoint lines; the board's serial log is compared with
// simulator/scenarios/cpu-replay.expected.
struct ReplayEvent {
  uint64_t at_ms = 0;  // relative to Start()
  std::vector<uint8_t> bytes;
  std::optional<std::array<uint8_t, protocol::kSessionIdBytes>> start_session;
};
struct ReplayCheckpoint {
  uint64_t at_ms = 0;
  const char* label = "";
};
struct CpuReplayScript {
  std::vector<ReplayEvent> events;
  std::vector<ReplayCheckpoint> checkpoints;
  uint64_t duration_ms = 0;
};

CpuReplayScript MakeCpuReplayScript();

// One line per checkpoint: "<label> | <connection> | <clock> | <value> |
// <note> | dimmed=<0|1>".
std::string FormatCheckpoint(const char* label, const CpuView& view);

class CpuReplay {
 public:
  CpuReplay(const MonotonicClock& clock, uint32_t boot_identity)
      : clock_(clock), feed_(clock, boot_identity), script_(MakeCpuReplayScript()) {}

  void Start();
  // Applies every event that is due, then returns the next due checkpoint
  // label (nullptr when none). Call repeatedly until it returns nullptr.
  const char* Poll();
  bool done() const { return started_ && next_checkpoint_ >= script_.checkpoints.size(); }
  uint64_t elapsed_ms() const { return clock_.NowMs() - start_ms_; }
  const HostFeed& feed() const { return feed_; }

 private:
  const MonotonicClock& clock_;
  HostFeed feed_;
  CpuReplayScript script_;
  uint64_t start_ms_ = 0;
  size_t next_event_ = 0;
  size_t next_checkpoint_ = 0;
  bool started_ = false;
};

}  // namespace firmware::domain
