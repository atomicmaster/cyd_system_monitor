// SPDX-License-Identifier: Apache-2.0
//
// M0 places an empty runnable image here. Display, touch, LED, audio, and
// storage drivers are F05/F06/F07 work under firmware/platform/esp32 and
// firmware/ui.
#include <cstdlib>

#include "firmware/domain/profile_check.hpp"

extern "C" void app_main(void) {
  // An unsupported profile geometry must fail clearly rather than boot into
  // an unvalidated layout.
  if (!firmware::domain::ValidateActiveProfile()) {
    abort();
  }
}
