// SPDX-License-Identifier: GPL-3.0-only
#include <cstdio>
#include <string>
#include <vector>

#include "../card_scenario.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/agents_bar.hpp"
#include "firmware/domain/coverage.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/link_view.hpp"
#include "firmware/ui/system/link/link_screen.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace domain = firmware::domain;
namespace proto = cyd::protocol;

constexpr uint64_t kGB = 1000ULL * 1000 * 1000;
constexpr uint64_t kGiB = 1ULL << 30;

// Every metric group live, as a busy laptop on WiFi would report it.
proto::SnapshotGroups LiveGroups(std::array<proto::AgentState, proto::kAgentSlots> agents) {
  proto::SnapshotGroups groups;
  groups.ram = proto::RamSnapshot{proto::Availability::kAvailable, 11 * kGiB, 16 * kGiB, 0,
                                  proto::Freshness::kFresh};
  groups.storage =
      proto::StorageSnapshot{proto::Availability::kAvailable, 500 * kGB,  200 * kGB, 0,
                             proto::Availability::kAvailable, 12'300'000, 4'500'000, 0};
  proto::BatterySnapshot battery;
  battery.charge_availability = proto::Availability::kAvailable;
  battery.charge_per_mille = 870;
  battery.state = proto::BatteryState::kDischarging;
  battery.time_remaining_min = 312;
  battery.condition_availability = proto::Availability::kAvailable;
  battery.max_capacity_per_mille = 910;
  battery.condition = proto::BatteryCondition::kNormal;
  battery.cycle_count = 214;
  groups.battery = battery;
  groups.agents = proto::AgentsSnapshot{proto::Availability::kAvailable, 0, agents};
  proto::NetworkSnapshot network;
  network.traffic_availability = proto::Availability::kAvailable;
  network.route_kind = proto::RouteKind::kWifi;
  network.rx_bytes_per_s = 1'200'000;
  network.tx_bytes_per_s = 340'000;
  network.context_availability = proto::AssociationContext::kAvailable;
  network.coverage = proto::WifiCoverage::kObservable;
  network.channel = 6;
  network.ssid = {'H', 'o', 'm', 'e', 'N', 'e', 't'};
  groups.network = network;
  return groups;
}

class Preview {
 public:
  explicit Preview(lv_obj_t* screen)
      : feed_(clock_, 1), ui_(screen, [](bool) {}, [](const domain::DisplaySettings&) {}, {}) {
    std::array<uint8_t, proto::kSessionIdBytes> session{};
    for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
    feed_.StartSession(session);
  }

  void Push(uint64_t ms, uint32_t sequence,
            std::array<proto::AgentState, proto::kAgentSlots> agents = {
                proto::AgentState::kNotRunning, proto::AgentState::kRunning,
                proto::AgentState::kNotRunning}) {
    clock_.Set(ms);
    feed_.Push(SnapshotFrame(sequence, LiveGroups(agents)));
  }

  // Renders at `ms`. A non-empty `clock` or `connection` replaces the domain's
  // text, to show a Host-supplied time or a worst-case width.
  void Render(uint64_t ms, const std::string& coverage, const std::string& clock_text = {},
              const std::string& connection = {}, const domain::LinkView& link = {}) {
    clock_.Set(ms);
    feed_.Tick();
    std::vector<domain::MetricCardView> cards;
    for (const auto& card : domain::Cards()) cards.push_back(card.make(feed_.state(), ms));
    auto status = domain::MakeCpuView(feed_.state(), ms);
    if (!clock_text.empty()) status.clock = clock_text;
    if (!connection.empty()) status.connection = connection;
    ui_.Update(status, cards, link, domain::MakeAgentsBar(feed_.state(), ms), coverage);
  }

 private:
  ScriptedClock clock_;
  domain::HostFeed feed_;
  firmware::ui::system::LinkScreen ui_;
};

std::string Coverage(domain::RadioState wifi, domain::RadioState ble,
                     std::optional<domain::Radio> current, uint8_t channel) {
  domain::CoverageView view;
  view.wifi.state = wifi;
  view.wifi.last_channel = channel;
  view.ble.state = ble;
  view.current = current;
  return domain::CoverageStatusText(view);
}

}  // namespace

// Writes the Overview in representative states as BMPs (3x scale) to `dir`,
// for reviewing layout by eye. Not part of `./dev check`.
int RunPreview(const std::string& dir) {
  lv_obj_t* screen = InitLvglHeadless();
  int failures = 0;
  const auto save = [&](const char* name) {
    // Let animations (the 300 ms stale fade) finish so the image shows their end state.
    for (int i = 0; i < 10; ++i) {
      lv_tick_inc(100);
      lv_timer_handler();
    }
    const std::string path = dir + "/" + name + ".bmp";
    if (SaveScreenshot(path, 3)) {
      std::printf("wrote %s\n", path.c_str());
    } else {
      std::fprintf(stderr, "preview: could not write %s\n", path.c_str());
      ++failures;
    }
  };
  using domain::Radio;
  using domain::RadioState;
  {
    Preview preview(screen);
    preview.Render(1000, Coverage(RadioState::kNoData, RadioState::kNoData, {}, 0));
    save("1-waiting");
  }
  {
    Preview preview(screen);
    preview.Push(2000, 1);
    preview.Render(2000, Coverage(RadioState::kObserving, RadioState::kObserving, Radio::kWifi, 6),
                   "10:42 PM");
    save("2-live");
  }
  {
    Preview preview(screen);
    preview.Push(2000, 1);
    preview.Render(8000, Coverage(RadioState::kObserving, RadioState::kGap, Radio::kBle, 6),
                   "10:42 PM");
    save("3-stale");
  }
  {
    // A future activity adapter: Codex busy, Claude activity unknown, Grok idle.
    Preview preview(screen);
    preview.Push(2000, 1,
                 {proto::AgentState::kBusy, proto::AgentState::kRunning, proto::AgentState::kIdle});
    preview.Render(2000, Coverage(RadioState::kObserving, RadioState::kObserving, Radio::kBle, 6),
                   "22:42");
    save("4-agents-busy-unknown-idle");
    // The busiest Overview (every card, three animated badges) against the
    // board's LVGL pool (firmware/ui/lv_conf.h).
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    std::printf("LVGL pool: %u of %u bytes used, peak %u\n",
                static_cast<unsigned>(memory.total_size - memory.free_size),
                static_cast<unsigned>(memory.total_size), static_cast<unsigned>(memory.max_used));
  }
  {
    Preview preview(screen);
    domain::LinkView link;
    link.fault = "Incompatible protocol: Host data rejected";
    preview.Render(1000, Coverage(RadioState::kFault, RadioState::kGap, Radio::kWifi, 13),
                   "Time unavailable", "Incompatible", link);
    save("5-status-bar-worst-case");
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace simulator::scenarios
