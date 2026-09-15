// SPDX-License-Identifier: GPL-3.0-only
#include <cstdio>
#include <cstdint>
#include <string>

#include "firmware/domain/generated/profile.hpp"
#include "firmware/domain/profile_check.hpp"

namespace {

// A controllable clock the smoke scenario advances explicitly, so replay
// stays deterministic instead of depending on wall-clock timing.
class SimClock {
 public:
  explicit SimClock(uint64_t start_ms) : now_ms_(start_ms) {}
  uint64_t NowMs() const { return now_ms_; }
  void AdvanceMs(uint64_t delta_ms) { now_ms_ += delta_ms; }

 private:
  uint64_t now_ms_;
};

int RunSmoke() {
  if (!firmware::domain::ValidateActiveProfile()) {
    std::fprintf(stderr, "smoke: active profile geometry is unsupported\n");
    return 1;
  }

  SimClock clock(0);
  clock.AdvanceMs(1000);

  std::printf(
      "simulator smoke: profile=%s geometry=%dx%d clock_ms=%llu\n",
      firmware::domain::profile::kProfileId, firmware::domain::profile::kLogicalWidth,
      firmware::domain::profile::kLogicalHeight,
      static_cast<unsigned long long>(clock.NowMs()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  if (scenario == "smoke") {
    return RunSmoke();
  }
  std::fprintf(stderr, "usage: simulator <scenario>\n  scenario: smoke\n");
  return 2;
}
