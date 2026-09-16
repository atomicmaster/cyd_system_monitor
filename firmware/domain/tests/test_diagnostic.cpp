// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>

#include "firmware/domain/diagnostic.hpp"
#include "firmware/domain/reset_reason.hpp"

namespace {
int failures = 0;
void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

int main() {
  using firmware::domain::DiagnosticState;
  using firmware::domain::FindPeripheral;
  using firmware::domain::PeripheralStatus;
  using firmware::domain::ResetReason;
  using firmware::domain::ToString;

  DiagnosticState state;
  state.profile_id = "lcdwiki-esp32-32e-2.8";
  state.build_identity = "test-build";
  state.reset_reason = ResetReason::ExternalReset;
  state.peripherals = {
      PeripheralStatus{"touch", true, "calibrated"},
      PeripheralStatus{"microsd", false, "not present (idle)"},
  };

  const auto* touch = FindPeripheral(state, "touch");
  Expect(touch != nullptr, "FindPeripheral finds an existing row by name");
  Expect(touch != nullptr && touch->present, "found row reports its actual present value");
  Expect(touch != nullptr && touch->detail == "calibrated", "found row reports its actual detail text");

  const auto* sd = FindPeripheral(state, "microsd");
  Expect(sd != nullptr, "FindPeripheral finds a present==false row");
  Expect(sd != nullptr && !sd->present,
         "an absent-but-probed row is distinct from a missing row: present is false, not null");

  Expect(FindPeripheral(state, "battery") == nullptr,
         "FindPeripheral returns nullptr for a name with no matching row");

  // Every ResetReason must have a distinct, non-empty string -- the UART
  // report and diagnostic screen both depend on this.
  const ResetReason all_reasons[] = {
      ResetReason::PowerOn,      ResetReason::ExternalReset, ResetReason::Watchdog,
      ResetReason::Brownout,     ResetReason::DeepSleepWake, ResetReason::Software,
      ResetReason::Unknown,
  };
  for (size_t i = 0; i < sizeof(all_reasons) / sizeof(all_reasons[0]); ++i) {
    const char* text = ToString(all_reasons[i]);
    Expect(text != nullptr && std::strlen(text) > 0, "ToString returns non-empty text for every reason");
    for (size_t j = 0; j < i; ++j) {
      Expect(std::strcmp(text, ToString(all_reasons[j])) != 0,
             "ToString returns a distinct string per reason");
    }
  }

  if (failures == 0) {
    std::printf("firmware_domain_diagnostic_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_diagnostic_tests: %d failure(s)\n", failures);
  return 1;
}
