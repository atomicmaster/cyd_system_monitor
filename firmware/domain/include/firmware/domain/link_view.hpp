// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "firmware/domain/host_link.hpp"

namespace firmware::domain {

// Pairing prompt and About/diagnostics text as plain strings, so the board
// and simulator render identically and tests need no LVGL.
struct LinkView {
  bool prompt_visible = false;  // touch confirmation is pending
  std::string prompt;           // shown with Confirm/Decline while pending
  std::string about;            // versions and pairing status, one per line
  std::string fault;            // visible incompatibility/refusal cause, else empty
};

LinkView MakeLinkView(const HostLink& link, bool paired);

}  // namespace firmware::domain
