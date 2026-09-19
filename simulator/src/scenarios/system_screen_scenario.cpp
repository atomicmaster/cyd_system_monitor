// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>
#include <vector>

#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/host_feed.hpp"
#include "firmware/ui/system/system_screen.hpp"
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
  uint64_t now_ms_ = 1000;
};

constexpr uint64_t kGiB = 1ULL << 30;

std::vector<uint8_t> Frame(uint32_t sequence, const proto::RamSnapshot* ram) {
  proto::Envelope envelope;
  envelope.type = proto::MessageType::kSnapshot;
  envelope.sequence = sequence;
  for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
  const proto::CpuSnapshot cpu{proto::Availability::kAvailable, 480, 0, proto::Freshness::kFresh,
                               0};
  std::vector<uint8_t> frame;
  proto::EncodeSnapshot(envelope, cpu, ram, &frame);
  return frame;
}

bool Has(lv_obj_t* label, const char* text) {
  return std::strstr(lv_label_get_text(label), text) != nullptr;
}

}  // namespace

// Draws the System shell through the card registry: CPU and RAM side by side
// under the status bar, then RAM's missing-data and stale behaviour.
int RunSystemScreen() {
  ScenarioResult result;
  lv_obj_t* screen = InitLvglHeadless();
  firmware::ui::system::SystemScreen ui(screen);
  ScriptedClock clock;
  domain::HostFeed feed(clock, 1);
  std::array<uint8_t, proto::kSessionIdBytes> session{};
  for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
  feed.StartSession(session);

  const auto render = [&](uint64_t now_ms) {
    clock.Set(now_ms);
    feed.Tick();
    std::vector<domain::MetricCardView> cards;
    for (const auto& card : domain::Cards()) cards.push_back(card.make(feed.state(), now_ms));
    ui.Update(domain::MakeCpuView(feed.state(), now_ms), cards);
    RenderOnce();
  };

  render(1000);
  result.Expect(ui.card_count() == domain::Cards().size(), "one card widget per registered card");
  result.Expect(Has(ui.card(0).title_label(), "CPU") && Has(ui.card(1).title_label(), "RAM"),
                "cards appear in registry order: CPU, RAM");
  result.Expect(Has(ui.card(1).value_label(), "--") && !ui.card(1).dimmed(),
                "RAM shows no value before any Host data");

  const proto::RamSnapshot ram{proto::Availability::kAvailable, 8 * kGiB, 16 * kGiB, 0,
                               proto::Freshness::kFresh};
  clock.Set(2000);
  feed.Push(Frame(1, &ram));
  render(2000);
  result.Expect(Has(ui.card(0).value_label(), "48%"), "CPU card shows the live CPU value");
  result.Expect(Has(ui.card(1).value_label(), "8.0 / 16.0 GB") && !ui.card(1).dimmed(),
                "RAM card shows used / total and is not dimmed");
  result.Expect(Has(ui.connection_label(), "Connected"), "status bar shows Connected");

  const proto::RamSnapshot unavailable{proto::Availability::kError, 0, 0, 0,
                                       proto::Freshness::kFresh};
  clock.Set(3000);
  feed.Push(Frame(2, &unavailable));
  render(3000);
  result.Expect(Has(ui.card(1).value_label(), "--") && Has(ui.card(1).note_label(), "Read error"),
                "a RAM read error shows -- and its cause, never zero");
  result.Expect(Has(ui.card(0).value_label(), "48%"), "CPU is unaffected by the RAM error");

  clock.Set(4000);
  feed.Push(Frame(3, &ram));
  clock.Set(4000);
  render(4000);
  result.Expect(Has(ui.card(1).value_label(), "8.0 / 16.0 GB"), "RAM recovers when readable again");

  render(7100);  // 3.1 s since the last frame
  result.Expect(Has(ui.card(1).value_label(), "Last 8.0 / 16.0 GB") && ui.card(1).dimmed(),
                "stale RAM keeps its last value, labelled Last and dimmed");
  return result.Finish("simulator system-screen");
}

}  // namespace simulator::scenarios
