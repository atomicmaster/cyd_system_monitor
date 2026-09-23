// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/coverage.hpp"
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

  domain::ObservationScheduler scheduler;
  domain::CoverageLedger ledger;
  const auto run_windows = [&](uint64_t now_ms, int count, bool lossy) {
    for (int i = 0; i < count; ++i) {
      const auto window = scheduler.NextWindow(now_ms);
      if (!window) break;
      ledger.Record({*window, lossy ? 0u : window->planned_ms, 0u});
      now_ms += window->planned_ms;
    }
  };

  const auto render = [&](uint64_t now_ms) {
    clock.Set(now_ms);
    feed.Tick();
    std::vector<domain::MetricCardView> cards;
    for (const auto& card : domain::Cards()) cards.push_back(card.make(feed.state(), now_ms));
    ui.Update(domain::MakeCpuView(feed.state(), now_ms), cards,
              domain::CoverageStatusText(domain::MakeCoverageView(scheduler, ledger, now_ms)));
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

  // Coverage stays in the persistent bar and never reads as quiet.
  result.Expect(Has(ui.coverage_label(), "WiFi ? BLE ?"),
                "coverage is unknown, not quiet, before the radio reports");
  run_windows(8000, 2, false);
  render(8500);
  result.Expect(Has(ui.coverage_label(), "WiFi 100% BLE 100%"),
                "reported observed time appears in the status bar");
  run_windows(8500, 2, true);
  render(9000);
  result.Expect(Has(ui.coverage_label(), "gap"), "a lost window shows as a gap in the status bar");
  scheduler.Configure({.wifi_enabled = false});
  render(9100);
  result.Expect(Has(ui.coverage_label(), "WiFi off"), "a disabled radio reads off");
  scheduler.Configure({.wifi_enabled = false, .ble_enabled = false});
  render(9200);
  result.Expect(Has(ui.coverage_label(), "Host only"), "both radios off reads Host only");
  render(200000);
  result.Expect(Has(ui.coverage_label(), "Host only") && !Has(ui.connection_label(), "Connected"),
                "coverage stays in the bar while the Host link goes stale");

  // Six cards (the Overview's 2x3 grid) fit the 320x240 display with no
  // scrolling, and realistic worst-case text fits its card without clipping.
  {
    const char* titles[] = {"System", "Storage", "Network", "Agents", "WiFi", "BLE"};
    std::vector<domain::MetricCardView> six;
    for (const char* title : titles) {
      six.push_back({.title = title,
                     .value = "Last 745.2 / 994.6 GB",
                     .note = "Last R99.9M/s W99.9M/s",
                     .has_value = true});
    }
    ui.Update(domain::MakeCpuView(feed.state(), 7100), six);
    RenderOnce();
    lv_obj_update_layout(screen);
    bool inside = ui.card_count() == 6;
    bool clipped = false;
    int32_t bottom_most = 0;
    for (size_t i = 0; i < ui.card_count(); ++i) {
      lv_area_t box;
      lv_obj_get_coords(lv_obj_get_parent(ui.card(i).title_label()), &box);
      inside = inside && box.x1 >= 0 && box.x2 < 320 && box.y1 >= 0 && box.y2 < 240;
      bottom_most = std::max(bottom_most, box.y2);
      for (lv_obj_t* label : {ui.card(i).value_label(), ui.card(i).note_label()}) {
        lv_point_t text;
        lv_text_get_size(&text, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0), 0,
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        clipped = clipped || text.x > lv_obj_get_content_width(label);
      }
    }
    result.Expect(inside, "six cards sit inside the 320x240 display");
    result.Expect(bottom_most == 239 || bottom_most < 240, "the grid ends above the display edge");
    result.Expect(
        !lv_obj_has_flag(screen, LV_OBJ_FLAG_SCROLLABLE) || lv_obj_get_scroll_bottom(screen) == 0,
        "the Overview does not scroll");
    result.Expect(!clipped, "realistic worst-case card text fits without clipping");
  }

  // Every Battery card text variant, laid out on a real card widget, fits the
  // 148 px content width of both lines (value at 14 pt, note at 12 pt).
  {
    size_t battery_index = 0;
    for (size_t i = 0; i < domain::Cards().size(); ++i) {
      if (std::string(domain::Cards()[i].id) == "battery") battery_index = i;
    }
    const uint8_t states[] = {0, 1, 2, 3, 4};
    const uint8_t conditions[] = {0, 1, 2, 3};
    bool fits = true;
    int variants = 0;
    for (const uint32_t age : {0u, 200'000u}) {  // fresh, and both halves stale ("Last ...")
      for (const uint8_t state : states) {
        for (const uint8_t condition : conditions) {
          for (const uint16_t per_mille : {uint16_t{1000}, uint16_t{870}, uint16_t{5}}) {
            proto::BatterySnapshot g;
            g.charge_availability = proto::Availability::kAvailable;
            g.charge_per_mille = per_mille;
            g.state = state;
            g.charge_age_ms = age;
            g.condition_availability = proto::Availability::kAvailable;
            g.max_capacity_per_mille = per_mille;
            g.condition = condition;
            g.condition_age_ms = age * 100;
            proto::Envelope envelope;
            envelope.type = proto::MessageType::kSnapshot;
            envelope.sequence = static_cast<uint32_t>(100 + variants);
            envelope.session = session;
            const proto::CpuSnapshot cpu{proto::Availability::kAvailable, 480, 0,
                                         proto::Freshness::kFresh, 0};
            std::vector<uint8_t> frame;
            proto::EncodeSnapshot(envelope, cpu, nullptr, nullptr, &g, &frame);
            clock.Set(1'000'000);
            feed.Push(frame);
            render(1'000'000);
            ++variants;
            for (lv_obj_t* label :
                 {ui.card(battery_index).value_label(), ui.card(battery_index).note_label()}) {
              lv_point_t text;
              lv_text_get_size(&text, lv_label_get_text(label),
                               lv_obj_get_style_text_font(label, 0), 0, 0, LV_COORD_MAX,
                               LV_TEXT_FLAG_NONE);
              if (text.x > 148) {
                std::printf("battery text too wide (%d px): %s\n", static_cast<int>(text.x),
                            lv_label_get_text(label));
                fits = false;
              }
            }
          }
        }
      }
    }
    result.Expect(variants == 120 && fits,
                  "every Battery value and note variant fits 148 px (fresh and Last)");
  }
  // Every Agents card text variant (all 64 state combinations, fresh and
  // "Last", plus each unavailable wording) fits the 148 px of both lines.
  {
    size_t agents_index = 0;
    for (size_t i = 0; i < domain::Cards().size(); ++i) {
      if (std::string(domain::Cards()[i].id) == "agents") agents_index = i;
    }
    bool fits = true;
    int variants = 0;
    int widest_value = 0, widest_note = 0;
    const auto measure = [&](const char* what) {
      for (lv_obj_t* label :
           {ui.card(agents_index).value_label(), ui.card(agents_index).note_label()}) {
        lv_point_t text;
        lv_text_get_size(&text, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0), 0,
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        (label == ui.card(agents_index).value_label() ? widest_value : widest_note) =
            std::max(label == ui.card(agents_index).value_label() ? widest_value : widest_note,
                     static_cast<int>(text.x));
        if (text.x > 148) {
          std::printf("agents text too wide (%d px, %s): %s\n", static_cast<int>(text.x), what,
                      lv_label_get_text(label));
          fits = false;
        }
      }
    };
    for (const uint32_t age : {0u, 20'000u}) {
      for (uint8_t a = 0; a < 4; ++a) {
        for (uint8_t b = 0; b < 4; ++b) {
          for (uint8_t c = 0; c < 4; ++c) {
            proto::AgentsSnapshot g;
            g.availability = proto::Availability::kAvailable;
            g.source_age_ms = age;
            g.state[0] = a;
            g.state[1] = b;
            g.state[2] = c;
            proto::Envelope envelope;
            envelope.type = proto::MessageType::kSnapshot;
            envelope.sequence = static_cast<uint32_t>(1000 + variants);
            envelope.session = session;
            const proto::CpuSnapshot cpu{proto::Availability::kAvailable, 480, 0,
                                         proto::Freshness::kFresh, 0};
            std::vector<uint8_t> frame;
            proto::EncodeSnapshot(envelope, cpu, nullptr, nullptr, nullptr, &g, &frame);
            clock.Set(2'000'000);
            feed.Push(frame);
            render(2'000'000);
            ++variants;
            measure("states");
          }
        }
      }
    }
    for (uint8_t availability = 1; availability <= 4; ++availability) {
      proto::AgentsSnapshot g;
      g.availability = static_cast<proto::Availability>(availability);
      proto::Envelope envelope;
      envelope.type = proto::MessageType::kSnapshot;
      envelope.sequence = static_cast<uint32_t>(1000 + variants);
      envelope.session = session;
      const proto::CpuSnapshot cpu{proto::Availability::kAvailable, 480, 0,
                                   proto::Freshness::kFresh, 0};
      std::vector<uint8_t> frame;
      proto::EncodeSnapshot(envelope, cpu, nullptr, nullptr, nullptr, &g, &frame);
      clock.Set(2'000'000);
      feed.Push(frame);
      render(2'000'000);
      ++variants;
      measure("unavailable");
    }
    std::printf("agents widest value %d px, widest note %d px\n", widest_value, widest_note);
    result.Expect(variants == 132 && fits,
                  "every Agents value and note variant fits 148 px (fresh and Last)");
  }
  // Every Network card text variant fits the 148 px of both lines: each route
  // kind (tunnel or not), coverage and channel up to the wire maximum of 255,
  // fresh and stale halves, the widest rates, and each unavailable wording.
  {
    size_t network_index = 0;
    for (size_t i = 0; i < domain::Cards().size(); ++i) {
      if (std::string(domain::Cards()[i].id) == "network") network_index = i;
    }
    bool fits = true;
    int variants = 0;
    int widest_value = 0, widest_note = 0;
    const auto measure = [&](const char* what) {
      for (lv_obj_t* label :
           {ui.card(network_index).value_label(), ui.card(network_index).note_label()}) {
        lv_point_t text;
        lv_text_get_size(&text, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0), 0,
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        (label == ui.card(network_index).value_label() ? widest_value : widest_note) =
            std::max(label == ui.card(network_index).value_label() ? widest_value : widest_note,
                     static_cast<int>(text.x));
        if (text.x > 148) {
          std::printf("network text too wide (%d px, %s): %s\n", static_cast<int>(text.x), what,
                      lv_label_get_text(label));
          fits = false;
        }
      }
    };
    const auto push_network = [&](const proto::NetworkSnapshot& g) {
      proto::Envelope envelope;
      envelope.type = proto::MessageType::kSnapshot;
      envelope.sequence = static_cast<uint32_t>(3000 + variants);
      envelope.session = session;
      const proto::CpuSnapshot cpu{proto::Availability::kAvailable, 480, 0,
                                   proto::Freshness::kFresh, 0};
      std::vector<uint8_t> frame;
      proto::EncodeSnapshot(envelope, cpu, nullptr, nullptr, nullptr, nullptr, &g, &frame);
      clock.Set(3'000'000);
      feed.Push(frame);
      render(3'000'000);
      ++variants;
    };
    const proto::RouteKind kinds[] = {proto::RouteKind::kUnknown, proto::RouteKind::kWifi,
                                      proto::RouteKind::kEthernet, proto::RouteKind::kOther,
                                      proto::RouteKind::kNone};
    const proto::WifiCoverage coverages[] = {proto::WifiCoverage::kUnknown,
                                             proto::WifiCoverage::kObservable,
                                             proto::WifiCoverage::kOutsideSupported};
    for (const proto::RouteKind kind : kinds) {
      for (const bool tunnel : {false, true}) {
        if (tunnel && (kind == proto::RouteKind::kUnknown || kind == proto::RouteKind::kNone))
          continue;  // the codec refuses a tunnel with no route under it
        for (const proto::WifiCoverage coverage : coverages) {
          for (const uint8_t channel : {0, 6, 14, 149, 255}) {
            if (coverage == proto::WifiCoverage::kUnknown && channel != 0) continue;
            for (const uint64_t rate : {uint64_t{0}, uint64_t{999}, uint64_t{1'234'567},
                                        uint64_t{999'400'000'000ULL}, UINT64_MAX}) {
              for (const uint32_t context_age : {0u, 20'000u}) {
                for (const uint32_t traffic_age : {0u, 4'000u}) {
                  proto::NetworkSnapshot g;
                  g.traffic_availability = kind == proto::RouteKind::kNone
                                               ? proto::Availability::kAbsent
                                               : proto::Availability::kAvailable;
                  g.route_kind = kind;
                  g.via_tunnel = tunnel;
                  g.rx_bytes_per_s =
                      g.traffic_availability == proto::Availability::kAvailable ? rate : 0;
                  g.tx_bytes_per_s = g.rx_bytes_per_s;
                  g.traffic_age_ms = traffic_age;
                  g.context_age_ms = context_age;
                  g.coverage = coverage;
                  g.channel = channel;
                  push_network(g);
                  measure("route");
                }
              }
            }
          }
        }
      }
    }
    for (uint8_t availability = 1; availability <= 4; ++availability) {
      proto::NetworkSnapshot g;
      g.traffic_availability = static_cast<proto::Availability>(availability);
      g.route_kind = proto::RouteKind::kWifi;
      push_network(g);
      measure("unavailable");
    }
    std::printf("network widest value %d px, widest note %d px\n", widest_value, widest_note);
    result.Expect(variants > 500 && fits,
                  "every Network value and note variant fits 148 px (fresh and Last)");
  }
  return result.Finish("simulator system-screen");
}

}  // namespace simulator::scenarios
