// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/host_feed.hpp"
#include "firmware/ui/system/cpu/cpu_card.hpp"
#include "protocol.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace domain = firmware::domain;
namespace proto = cyd::protocol;

class ScriptedClock : public domain::MonotonicClock {
 public:
  uint64_t NowMs() const override { return now_ms_; }
  void Set(uint64_t now_ms) { now_ms_ = now_ms; }

 private:
  uint64_t now_ms_ = 0;
};

std::array<uint8_t, proto::kSessionIdBytes> SessionId(uint8_t first) {
  std::array<uint8_t, proto::kSessionIdBytes> id{};
  for (uint8_t i = 0; i < id.size(); ++i) id[i] = i;
  id[0] = first;
  return id;
}

struct Snapshot {
  uint32_t sequence = 1;
  proto::Availability availability = proto::Availability::kAvailable;
  uint16_t per_mille = 0;
  uint32_t source_age_ms = 0;
  proto::Freshness freshness = proto::Freshness::kFresh;
  uint8_t session = 1;
  std::optional<uint64_t> utc_ms;
  uint8_t major = 1;
};

std::vector<uint8_t> Frame(const Snapshot& s) {
  proto::Envelope envelope;
  envelope.version.major = s.major;
  envelope.type = proto::MessageType::kSnapshot;
  envelope.session = SessionId(s.session);
  envelope.sequence = s.sequence;
  envelope.utc_ms = s.utc_ms;
  std::vector<uint8_t> frame;
  proto::EncodeCpuSnapshot(envelope, {s.availability, s.per_mille, s.source_age_ms, s.freshness, 3},
                           &frame);
  return frame;
}

std::string Text(lv_obj_t* label) { return lv_label_get_text(label); }

}  // namespace

int RunCpuLifecycle() {
  ScenarioResult result;
  lv_obj_t* screen = InitLvglHeadless();
  firmware::ui::system::CpuScreen ui(screen);
  ScriptedClock clock;
  domain::HostFeed feed(clock, 1);
  feed.StartSession(SessionId(1));

  std::ostringstream transcript;
  auto show = [&](const char* step) {
    feed.Tick();
    const auto view = domain::MakeCpuView(feed.state(), clock.NowMs());
    ui.Update(view);
    transcript << step << " | " << Text(ui.connection_label()) << " | " << Text(ui.clock_label())
               << " | " << Text(ui.value_label()) << " | " << Text(ui.note_label())
               << " | dimmed=" << ui.dimmed() << " | bar="
               << (lv_obj_has_flag(ui.bar(), LV_OBJ_FLAG_HIDDEN)
                       ? std::string("hidden")
                       : std::to_string(lv_bar_get_value(ui.bar())))
               << " | applied=" << feed.stats().applied << " rejected=" << feed.stats().rejected
               << "\n";
    RenderOnce();
    return view;
  };

  clock.Set(500);
  auto view = show("cold-boot");
  result.Expect(view.clock == "Time unavailable", "cold boot shows Time unavailable");
  result.Expect(view.value == "--" && !view.has_value, "cold boot has no CPU value");

  clock.Set(1000);
  feed.Push(Frame({.sequence = 1,
                   .availability = proto::Availability::kTemporarilyUnavailable,
                   .per_mille = 0}));
  view = show("first-unavailable-rate");
  result.Expect(view.value == "--" && view.note == "Unavailable" && !view.has_value,
                "an unavailable first rate is never shown as 0%");

  const struct {
    proto::Availability availability;
    const char* step;
    const char* note;
  } states[] = {{proto::Availability::kUnsupported, "state-unsupported", "Unsupported"},
                {proto::Availability::kAbsent, "state-absent", "Not present"},
                {proto::Availability::kError, "state-error", "Read error"}};
  uint32_t sequence = 2;
  for (const auto& state : states) {
    clock.Set(1000 + 1000 * sequence);
    feed.Push(
        Frame({.sequence = sequence++, .availability = state.availability, .per_mille = 999}));
    view = show(state.step);
    result.Expect(view.value == "--" && view.note == state.note,
                  "a non-available state shows its own wording and no value, even with a "
                  "nonzero payload");
  }

  clock.Set(6000);
  feed.Push(Frame({.sequence = sequence++,
                   .per_mille = 623,
                   .source_age_ms = 100,
                   .utc_ms = 86400000ULL * 20000 + (13 * 3600 + 7 * 60) * 1000ULL}));
  view = show("valid-live");
  result.Expect(view.value == "62%" && view.has_value && !view.dimmed,
                "a valid rate is shown live");
  result.Expect(view.clock == "13:07 UTC", "Host UTC replaces Time unavailable");
  result.Expect(feed.state().cpu.source_time_ms == 5900,
                "source time is receipt time minus the reported source age");

  clock.Set(8999);
  view = show("just-under-3s");
  result.Expect(!view.dimmed && view.value == "62%", "no dimming before three seconds");
  clock.Set(9000);
  view = show("stale-at-3s");
  result.Expect(view.dimmed && view.value == "Last 62%" && ui.dimmed(),
                "at three seconds the value dims and is labeled Last");
  result.Expect(view.bar_per_mille == 623, "a stale value is retained, not decayed to zero");
  result.Expect(view.connection == "Stale", "the connection is Stale before 15 seconds");
  clock.Set(20999);
  view = show("just-under-15s");
  result.Expect(view.connection == "Stale", "still stale just under 15 seconds");
  clock.Set(21000);
  view = show("disconnected-at-15s");
  result.Expect(view.connection == "Disconnected" && view.value == "Last 62%",
                "at 15 seconds the Host is disconnected and the last value is still labeled");

  clock.Set(22000);
  feed.Push(Frame({.sequence = sequence++, .per_mille = 410, .source_age_ms = 2500}));
  view = show("recovered-old-source");
  result.Expect(view.value == "41%" && !view.dimmed && view.connection == "Connected",
                "a fresh complete snapshot restores live state");
  result.Expect(feed.state().cpu.source_time_ms == 19500,
                "an old source timestamp stays old even though the snapshot arrived now");
  const uint64_t source_before_correction = feed.state().cpu.source_time_ms;

  clock.Set(23000);
  feed.Push(Frame({.sequence = sequence++,
                   .per_mille = 400,
                   .utc_ms = 86400000ULL * 20000 + (2 * 3600 + 30 * 60) * 1000ULL}));
  view = show("time-correction-backwards");
  result.Expect(view.clock == "02:30 UTC", "a time correction replaces the displayed clock");
  result.Expect(feed.state().cpu.source_time_ms == 23000 && source_before_correction == 19500,
                "monotonic evidence order is unaffected by a UTC correction");

  // Rejected input: transcript rows show applied unchanged, rejected growing.
  const auto good = Frame({.sequence = sequence, .per_mille = 700});
  feed.Push(Frame({.sequence = sequence - 1, .per_mille = 999}));
  view = show("reject-stale-sequence");
  result.Expect(feed.stats().last_error == proto::Error::kStaleSequence && view.value == "40%",
                "a repeated sequence is rejected without changing the display");
  feed.Push(Frame({.sequence = sequence + 5, .per_mille = 999, .session = 9}));
  view = show("reject-wrong-session");
  result.Expect(feed.stats().last_error == proto::Error::kWrongSession && view.value == "40%",
                "an old or foreign session is rejected without changing the display");
  auto damaged = good;
  damaged[12] ^= 0x10;
  feed.Push(damaged);
  view = show("reject-bad-crc");
  result.Expect(view.value == "40%", "a corrupted frame is dropped without changing the display");
  // Rescanning the damaged bytes may leave a false length waiting; the idle
  // gap before the next frame clears it.
  clock.Set(23400);
  view = show("idle-gap-clears-stall");
  result.Expect(view.value == "40%", "abandoning a stalled partial frame changes nothing");

  // Bytes may arrive in fragments; the accepted frame lands only when whole.
  for (std::size_t i = 0; i + 1 < good.size(); ++i)
    feed.Push(std::span<const uint8_t>(&good[i], 1));
  view = show("fragmented-partial");
  result.Expect(view.value == "40%", "an incomplete frame changes nothing");
  feed.Push(std::span<const uint8_t>(&good.back(), 1));
  view = show("fragmented-complete");
  result.Expect(view.value == "70%", "the completed fragmented frame is applied");

  feed.Push(Frame({.sequence = sequence + 1, .major = 2}));
  view = show("incompatible-major");
  result.Expect(view.connection == "Incompatible" && view.value == "Last 70%",
                "an incompatible major is visible and applies no metrics");

  // Reconnect: a new negotiated session restarts at sequence one.
  clock.Set(30000);
  feed.StartSession(SessionId(2));
  feed.Push(Frame({.sequence = 1, .per_mille = 250, .session = 2}));
  view = show("reconnect-new-session");
  result.Expect(view.value == "25%" && feed.state().connection == domain::ConnectionState::kLive,
                "a new session restarts sequences at one and restores live state");

  const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/cpu-lifecycle.expected";
  if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript.str();
  std::ifstream expected_file(path);
  std::stringstream expected;
  expected << expected_file.rdbuf();
  result.Expect(expected.str() == transcript.str(),
                "the rendered transcript matches simulator/scenarios/cpu-lifecycle.expected "
                "(rerun with CYD_UPDATE_EXPECTED=1 to review an intended change)");
  return result.Finish("simulator cpu-lifecycle");
}

}  // namespace simulator::scenarios
