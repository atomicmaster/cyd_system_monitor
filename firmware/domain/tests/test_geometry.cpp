// SPDX-License-Identifier: Apache-2.0
//
// Proves firmware/domain builds and runs independently of ESP-IDF. This is
// not a display or radio performance test.
#include <cstdio>

#include "firmware/domain/generated/profile.hpp"
#include "firmware/domain/geometry.hpp"
#include "firmware/domain/profile_check.hpp"

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
  Expect(firmware::domain::ValidateActiveProfile(),
         "active profile reports supported 320x240 landscape geometry");
  Expect(firmware::domain::profile::kLogicalWidth == 320, "logical width is 320");
  Expect(firmware::domain::profile::kLogicalHeight == 240, "logical height is 240");
  Expect(!firmware::domain::IsSupportedGeometry(firmware::domain::Geometry{240, 320}),
         "an unrotated portrait geometry is rejected");
  Expect(firmware::domain::IsConsistentRotation(240, 320, 320, 240),
         "a 90-degree rotated physical/logical pair is consistent");
  Expect(!firmware::domain::IsConsistentRotation(240, 320, 320, 320),
         "a physical/logical pair that is neither equal nor swapped is rejected");

  if (failures == 0) {
    std::printf("firmware_domain_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_tests: %d failure(s)\n", failures);
  return 1;
}
