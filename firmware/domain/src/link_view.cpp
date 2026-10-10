// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/link_view.hpp"

#include <cstdio>

namespace firmware::domain {
namespace {

std::string Version(uint8_t major, uint8_t minor) {
  return std::to_string(major) + "." + std::to_string(minor);
}

const char* FaultText(LinkFault fault) {
  switch (fault) {
    case LinkFault::kIncompatibleMajor:
      return "Incompatible protocol: Host data rejected";
    case LinkFault::kNotPairedWithHost:
      return "Another Host is paired: unpair it first";
    case LinkFault::kDeclined:
      return "Pairing declined or timed out";
    case LinkFault::kNone:
      break;
  }
  return "";
}

}  // namespace

LinkView MakeLinkView(const HostLink& link, bool paired) {
  const auto& d = link.diagnostics();
  LinkView view;
  if (const auto code = link.pending_code()) {
    char text[8];
    std::snprintf(text, sizeof(text), "%06u", static_cast<unsigned>(*code));
    view.prompt_visible = true;
    view.code = text;
  }
  view.details = {
      {"Supported", Version(d.supported_major, d.supported_minor)},
      {"Host", d.host_major ? Version(*d.host_major, d.host_minor.value_or(0)) : "none"},
      {"Negotiated", d.negotiated_minor ? Version(d.supported_major, *d.negotiated_minor) : "none"},
      {"Pairing", paired ? "Paired" : "Not paired"},
  };
  view.fault = FaultText(d.fault);
  return view;
}

}  // namespace firmware::domain
