// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/cpu_replay.hpp"
#include "firmware/ui/system/cpu/cpu_card.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace domain = firmware::domain;

class ScriptedClock : public domain::MonotonicClock {
 public:
  uint64_t NowMs() const override { return now_ms_; }
  void Advance(uint64_t delta_ms) { now_ms_ += delta_ms; }

 private:
  uint64_t now_ms_ = 1000;  // never zero: a real boot clock is already running
};

}  // namespace

int RunCpuReplay() {
  ScenarioResult result;
  lv_obj_t* screen = InitLvglHeadless();
  firmware::ui::system::CpuScreen ui(screen);
  ScriptedClock clock;
  domain::CpuReplay replay(clock, 1);
  replay.Start();

  std::ostringstream transcript;
  int checkpoints = 0;
  for (int step = 0; step < 400 && !replay.done(); ++step) {
    clock.Advance(100);
    while (const char* label = replay.Poll()) {
      const auto view = domain::MakeCpuView(replay.feed().state(), clock.NowMs());
      ui.Update(view);
      RenderOnce();
      transcript << domain::FormatCheckpoint(label, view) << "\n";
      ++checkpoints;
    }
  }
  result.Expect(replay.done(), "the replay script runs to its last checkpoint");
  result.Expect(checkpoints == 18, "every scripted checkpoint is reported exactly once");

  // The board logs the same lines over serial; this file is what a physical
  // `./dev run hardware -- cpu-replay` capture is compared against.
  const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/cpu-replay.expected";
  if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript.str();
  std::ifstream expected_file(path);
  std::stringstream expected;
  expected << expected_file.rdbuf();
  result.Expect(expected.str() == transcript.str(),
                "the checkpoint transcript matches simulator/scenarios/cpu-replay.expected "
                "(rerun with CYD_UPDATE_EXPECTED=1 to review an intended change)");
  return result.Finish("simulator cpu-replay");
}

}  // namespace simulator::scenarios
