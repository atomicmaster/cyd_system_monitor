// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdio>

namespace simulator {

// Same hand-rolled ok/failure-count convention firmware/domain/tests uses
// (see test_geometry.cpp): no external test framework, one process per
// scenario, nonzero exit on any assertion failure.
class ScenarioResult {
 public:
  void Expect(bool condition, const char* what) {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++failures_;
    }
  }

  int Finish(const char* scenario_name) const {
    if (failures_ == 0) {
      std::printf("%s: ok\n", scenario_name);
      return 0;
    }
    std::fprintf(stderr, "%s: %d failure(s)\n", scenario_name, failures_);
    return 1;
  }

 private:
  int failures_ = 0;
};

}  // namespace simulator
