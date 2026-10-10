// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

#include "firmware/domain/host_link.hpp"

namespace firmware::domain {

// One Host link detail row: "Negotiated" / "1.0".
struct LinkDetail {
  std::string label;
  std::string value;
};

// The pairing prompt and the Host link details as plain values, so the board
// and simulator render identically and tests need no LVGL.
struct LinkView {
  bool prompt_visible = false;      // touch confirmation is pending
  std::string code;                 // the six-digit pairing code while pending, else empty
  std::vector<LinkDetail> details;  // versions and pairing status, one row each
  std::string fault;                // visible incompatibility/refusal cause, else empty
};

LinkView MakeLinkView(const HostLink& link, bool paired);

}  // namespace firmware::domain
