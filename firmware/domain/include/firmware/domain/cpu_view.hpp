// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "firmware/domain/host_state.hpp"

namespace firmware::domain {

// Everything the CPU card and status bar display, as plain text and flags, so
// desktop and board render identically and tests need no LVGL.
struct CpuView {
  std::string value;       // "62%", "Last 62%", or "--"; a missing value is never "0%"
  std::string note;        // availability wording when there is no value, else empty
  std::string connection;  // persistent status text
  std::string clock;       // "Time unavailable" until the Host supplies UTC
  bool has_value = false;
  bool dimmed = false;
  uint16_t bar_per_mille = 0;  // 0 when there is no value; check has_value
};

CpuView MakeCpuView(const HostState& state, uint64_t now_ms);

}  // namespace firmware::domain
