// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/reset_reason.hpp"

namespace firmware::domain {

const char* ToString(ResetReason reason) {
  switch (reason) {
    case ResetReason::PowerOn:
      return "power-on";
    case ResetReason::ExternalReset:
      return "external-reset";
    case ResetReason::Watchdog:
      return "watchdog";
    case ResetReason::Brownout:
      return "brownout";
    case ResetReason::DeepSleepWake:
      return "deep-sleep-wake";
    case ResetReason::Software:
      return "software";
    case ResetReason::Unknown:
      return "unknown";
  }
  return "unknown";
}

}  // namespace firmware::domain
