// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/diagnostic.hpp"

namespace firmware::domain {

const PeripheralStatus* FindPeripheral(const DiagnosticState& state, std::string_view name) {
  for (const auto& peripheral : state.peripherals) {
    if (peripheral.name == name) {
      return &peripheral;
    }
  }
  return nullptr;
}

}  // namespace firmware::domain
