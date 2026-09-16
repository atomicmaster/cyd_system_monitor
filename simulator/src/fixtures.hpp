// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "firmware/domain/diagnostic.hpp"
#include "firmware/ui/capacity/capacity_slice.hpp"

namespace simulator::fixtures {

// A synthetic DiagnosticState standing in for what firmware/platform/esp32
// would populate from real hardware. Values here are fixture text, not
// measured behavior -- see docs/tickets/F05.md/F06.md/F07.md's Evidence
// sections for what remains unverified pending a physical board.
firmware::domain::DiagnosticState MakeDiagnosticStateFixture();

// A deliberately oversized CapacitySliceState: more alert/setting rows
// than firmware::domain::capacity's declared bounds allow, and
// longer-than-bounded label/detail strings, so the capacity scenario
// (F08a Work item 4) exercises the maximum object tree and maximum
// strings the capacity slice must still render as clamped/truncated
// rather than growing unbounded.
firmware::ui::capacity::CapacitySliceState MakeCapacitySliceStateFixture();

}  // namespace simulator::fixtures
