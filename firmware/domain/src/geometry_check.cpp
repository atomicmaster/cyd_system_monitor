// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/profile_check.hpp"

#include "firmware/domain/generated/profile.hpp"
#include "firmware/domain/geometry.hpp"

namespace firmware::domain {

bool ValidateActiveProfile() {
  return IsSupportedGeometry(
             Geometry{profile::kLogicalWidth, profile::kLogicalHeight}) &&
         IsConsistentRotation(profile::kDisplayWidth, profile::kDisplayHeight,
                               profile::kLogicalWidth, profile::kLogicalHeight);
}

}  // namespace firmware::domain
