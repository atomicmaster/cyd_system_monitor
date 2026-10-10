// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/card_registry.hpp"

#include <cmath>
#include <cstdio>
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

uint16_t LogRatePerMille(uint64_t bytes_per_s) {
  if (bytes_per_s < 100) return 0;
  const double level = (std::log10(static_cast<double>(bytes_per_s)) - 2.0) / 7.0;
  return static_cast<uint16_t>(std::fmin(1.0, level) * 1000.0);
}

std::string CompactRate(uint64_t bytes_per_s) {
  constexpr double kClamp = 999.4e9;
  const double value =
      bytes_per_s > static_cast<uint64_t>(kClamp) ? kClamp : static_cast<double>(bytes_per_s);
  char text[16];
  if (value < 1e3) {
    std::snprintf(text, sizeof text, "%uB", static_cast<unsigned>(value));
    return text;
  }
  const auto scaled = [&](double x, char unit) {
    std::snprintf(text, sizeof text, x < 9.95 ? "%.1f%c" : "%.0f%c", x, unit);
  };
  if (value < 999.5e3) {
    scaled(value / 1e3, 'K');
  } else if (value < 999.5e6) {
    scaled(value / 1e6, 'M');
  } else {
    scaled(value / 1e9, 'G');
  }
  return text;
}

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
