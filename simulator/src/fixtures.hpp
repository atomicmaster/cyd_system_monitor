// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "firmware/domain/diagnostic.hpp"

namespace simulator::fixtures {

// A synthetic DiagnosticState standing in for what firmware/platform/esp32
// would populate from real hardware. Values here are fixture text, not
// measured behavior -- see docs/tickets/F05.md/F06.md/F07.md's Evidence
// sections for what remains unverified pending a physical board.
firmware::domain::DiagnosticState MakeDiagnosticStateFixture();

}  // namespace simulator::fixtures
