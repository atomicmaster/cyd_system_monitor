// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/card_registry.hpp"

#include <iterator>

namespace firmware::domain {

#define CYD_CARD(id, make) MetricCardView make(const HostState&, uint64_t);
#include "firmware/domain/card_registry.def"
#undef CYD_CARD

namespace {
constexpr CardDescriptor kCards[] = {
#define CYD_CARD(id, make) {#id, make},
#include "firmware/domain/card_registry.def"
#undef CYD_CARD
};
}  // namespace

std::span<const CardDescriptor> Cards() { return kCards; }

const char* AvailabilityNote(protocol::Availability availability) {
  switch (availability) {
    case protocol::Availability::kAvailable:
      return "";
    case protocol::Availability::kUnsupported:
      return "Unsupported";
    case protocol::Availability::kAbsent:
      return "Not present";
    case protocol::Availability::kTemporarilyUnavailable:
      return "Unavailable";
    case protocol::Availability::kError:
      return "Read error";
  }
  return "";
}

}  // namespace firmware::domain
