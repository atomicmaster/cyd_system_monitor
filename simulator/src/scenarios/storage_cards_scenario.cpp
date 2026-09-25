// SPDX-License-Identifier: GPL-3.0-only
#include <string>
#include <vector>

#include "../card_scenario.hpp"
#include "protocol.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace proto = cyd::protocol;

constexpr uint64_t kGB = 1'000'000'000ULL;
constexpr auto kOk = proto::Availability::kAvailable;
constexpr auto kNo = proto::Availability::kTemporarilyUnavailable;

}  // namespace

int RunStorageCards() {
  CardScenario scenario("storage", "storage-cards", CardScenario::Columns::kBar);
  if (!scenario.ready()) return scenario.Finish();
  ScenarioResult& result = scenario.result();
  const auto at = [&](uint64_t ms, const char* label) { return scenario.At(ms, label); };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::StorageSnapshot* storage) {
    scenario.Push(ms, sequence, {.storage = *storage});
  };
  // Capacity 500 GB total, 200 GB free; I/O 12.3 MB/s read, 4.5 MB/s write.
  const auto snap = [](uint32_t capacity_age, proto::Availability io, uint64_t read,
                       uint64_t write) {
    return proto::StorageSnapshot{kOk, 500 * kGB, 200 * kGB, capacity_age, io, read, write, 0};
  };

  at(1000, "no Host data yet");
  auto s = snap(1500, kOk, 12'300'000, 4'500'000);
  push(2000, 1, &s);
  at(2000, "live: 300 of 500 GB used, R 12.3 MB/s W 4.5 MB/s");
  s = snap(2500, kNo, 0, 0);
  push(3000, 2, &s);
  auto view = at(3000, "I/O temporarily unavailable, capacity still live");
  result.Expect(view.has_value && !view.dimmed && view.note == "I/O Unavailable",
                "capacity stays live and undimmed while I/O is unavailable");
  s = snap(3500, kNo, 0, 0);
  push(4000, 3, &s);
  at(4000, "counter reset: I/O still unavailable, never a zero rate");
  s = snap(4500, kOk, 800, 0);
  push(5000, 4, &s);
  at(5000, "I/O back after re-baseline: 800 B/s read, a measured zero write");
  s = snap(5500, kOk, 2'500'000'000ULL, 999'960);
  push(6000, 5, &s);
  at(6000, "unit boundaries: 2.5 GB/s read, 999.96 KB/s write");

  // Capacity re-carried with an old sample age: not fresh just because the
  // snapshot is. Sampled at 10,000 - 30,000 (age), stale at 90 s after that.
  s = snap(30'000, kOk, 1'000'000, 2'000'000);
  push(100'000, 6, &s);  // capacity source time 70,000
  at(100'000, "capacity sampled 30 s ago, I/O live: both fresh");
  s = snap(89'000, kOk, 1'000'000, 2'000'000);
  push(160'000, 7, &s);  // source time 71,000
  at(160'000, "capacity 89 s old, I/O live: both fresh");
  s = snap(91'000, kOk, 1'000'000, 2'000'000);
  push(161'000, 8, &s);  // source time 70,000: the old sample again
  view = at(161'000, "capacity sample is 91 s old, I/O live: capacity stale, I/O live");
  result.Expect(view.dimmed && view.value.starts_with("Last ") && view.note.rfind("Last ", 0) != 0,
                "a snapshot re-carrying an old capacity sample does not make it fresh");

  // Capacity unavailable (statfs failed) while I/O is live.
  s = {proto::Availability::kError, 0, 0, 0, kOk, 100, 200, 0};
  push(162'000, 9, &s);
  view = at(162'000, "capacity read error, I/O live");
  result.Expect(!view.has_value && view.value == "--", "no capacity shows -- not zero");

  // A terabyte-class disk.
  s = {kOk, 2'000'000'000'000ULL, 500'000'000'000ULL, 0, kOk, 0, 0, 0};
  push(163'000, 10, &s);
  at(163'000, "2 TB disk, 1.5 TB used, idle: a zero rate is measured");

  // Everything stale: the frames stop.
  at(165'900, "last frame 2.9 s ago: still live");
  view = at(166'100, "last frame 3.1 s ago: I/O stale, capacity fresh, the link stale too");
  result.Expect(view.dimmed && view.value.starts_with("Last ") && view.note.starts_with("Last "),
                "stale retains both readings with Last and dims");
  at(179'000, "16 s without any frame: retained and dimmed");

  return scenario.Finish();
}

}  // namespace simulator::scenarios
