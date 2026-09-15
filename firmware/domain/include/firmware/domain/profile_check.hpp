// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::domain {

// Validates the active hardware profile's generated geometry against the
// domain's supported geometry set. Returns false rather than aborting so
// callers (firmware boot, simulator, tests) each choose how to fail clearly.
bool ValidateActiveProfile();

}  // namespace firmware::domain
