// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "../card_scenario.hpp"
#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/agents_bar.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/coverage.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/host_feed.hpp"
#include "firmware/ui/system/system_screen.hpp"
#include "firmware/ui/theme.hpp"
#include "protocol.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace domain = firmware::domain;
namespace proto = cyd::protocol;
using firmware::ui::system::MetricCard;
using firmware::ui::system::SystemScreen;
namespace theme = firmware::ui::theme;

constexpr uint64_t kGiB = 1ULL << 30;
constexpr uint64_t kGB = 1000ULL * 1000 * 1000;

bool Has(lv_obj_t* label, const char* text) {
  return std::strstr(lv_label_get_text(label), text) != nullptr;
}

int TextWidth(lv_obj_t* label, const std::string& text) {
  lv_point_t size;
  lv_text_get_size(&size, text.c_str(), lv_obj_get_style_text_font(label, 0), 0, 0, LV_COORD_MAX,
                   LV_TEXT_FLAG_NONE);
  return static_cast<int>(size.x);
}

// Every visible label under `object` lies inside the card's content box and
// its text fits its own width (each is a single clipped line), except
// `wrapped`, a dial's value, which is checked separately.
bool LabelsFit(lv_obj_t* object, const lv_area_t& box, lv_obj_t* wrapped, std::string* why) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
    lv_obj_t* child = lv_obj_get_child(object, static_cast<int32_t>(i));
    if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(child, &lv_label_class) && child != wrapped) {
      const std::string text = lv_label_get_text(child);
      lv_area_t area;
      lv_obj_get_coords(child, &area);
      if (area.x1 < box.x1 || area.x2 > box.x2 || area.y1 < box.y1 || area.y2 > box.y2) {
        *why = "outside the card \"" + text + "\"";
        return false;
      }
      if (TextWidth(child, text) > lv_obj_get_content_width(child)) {
        *why = "clipped \"" + text + "\"";
        return false;
      }
    }
    if (!LabelsFit(child, box, wrapped, why)) return false;
  }
  return true;
}

// Whether a card's text fits its widget without clipping. Every line is a
// single clipped label. A dial's value may wrap at spaces ("Last" / "62%") but
// no word may be wider than the ring's opening, and the wrapped text must fit
// the ring's inner height.
bool CardFits(const MetricCard& card, std::string* why) {
  lv_obj_update_layout(card.root());
  lv_area_t box;
  lv_obj_get_content_coords(card.root(), &box);
  lv_obj_t* value = card.dial() ? card.value_label() : nullptr;
  if (!LabelsFit(card.root(), box, value, why)) return false;
  if (value == nullptr) return true;
  const std::string value_text = lv_label_get_text(value);
  size_t start = 0;
  while (start <= value_text.size()) {
    const size_t end = std::min(value_text.find(' ', start), value_text.size());
    if (TextWidth(value, value_text.substr(start, end - start)) > lv_obj_get_width(value)) {
      *why = "dial word in \"" + value_text + "\"";
      return false;
    }
    start = end + 1;
  }
  lv_point_t wrapped;
  lv_text_get_size(&wrapped, value_text.c_str(), lv_obj_get_style_text_font(value, 0), 0, 0,
                   lv_obj_get_width(value), LV_TEXT_FLAG_NONE);
  if (wrapped.y > lv_obj_get_width(value)) {
    *why = "dial value too tall \"" + value_text + "\"";
    return false;
  }
  return true;
}

// Every visible child of `parent` lies inside its content area.
bool ChildrenInside(lv_obj_t* parent, std::string* why) {
  lv_obj_update_layout(parent);
  lv_area_t box;
  lv_obj_get_content_coords(parent, &box);
  for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
    lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
    if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
    const bool label = lv_obj_check_type(child, &lv_label_class);
    lv_area_t area;
    lv_obj_get_coords(child, &area);
    if (area.x1 < box.x1 || area.x2 > box.x2 || area.y1 < box.y1 || area.y2 > box.y2) {
      *why = (label ? std::string(lv_label_get_text(child)) : std::string("a badge")) + " x " +
             std::to_string(area.x1) + ".." + std::to_string(area.x2) + " y " +
             std::to_string(area.y1) + ".." + std::to_string(area.y2) + " in " +
             std::to_string(box.x1) + ".." + std::to_string(box.x2) + " y " +
             std::to_string(box.y1) + ".." + std::to_string(box.y2);
      return false;
    }
    if (label && TextWidth(child, lv_label_get_text(child)) > lv_obj_get_content_width(child)) {
      *why = lv_label_get_text(child);
      return false;
    }
  }
  return true;
}

// A HostFeed with an open session, a scripted clock, and the Overview drawn
// from it through the registry, exactly as the board draws it.
class Overview {
 public:
  explicit Overview(lv_obj_t* screen) : feed_(clock_, 1), ui_(screen) {
    std::array<uint8_t, proto::kSessionIdBytes> session{};
    for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
    feed_.StartSession(session);
  }

  void Push(uint64_t ms, const proto::SnapshotGroups& groups, uint16_t cpu_per_mille = 480) {
    clock_.Set(ms);
    proto::Envelope envelope;
    envelope.type = proto::MessageType::kSnapshot;
    envelope.sequence = ++sequence_;
    for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
    const proto::CpuSnapshot cpu{proto::Availability::kAvailable, cpu_per_mille, 0,
                                 proto::Freshness::kFresh, 0};
    std::vector<uint8_t> frame;
    proto::EncodeSnapshot(envelope, cpu, groups, &frame);
    feed_.Push(frame);
  }

  void Render(uint64_t ms, const domain::CoverageStatus& coverage = {},
              const std::string& notice = {}) {
    clock_.Set(ms);
    feed_.Tick();
    std::vector<domain::MetricCardView> cards;
    for (const auto& card : domain::Cards()) cards.push_back(card.make(feed_.state(), ms));
    ui_.Update(domain::MakeCpuView(feed_.state(), ms), cards,
               domain::MakeAgentsBar(feed_.state(), ms), coverage, notice);
    RenderOnce();
  }

  SystemScreen& ui() { return ui_; }
  const MetricCard& Card(const char* id) const {
    for (size_t i = 0; i < domain::Cards().size(); ++i) {
      if (std::string(domain::Cards()[i].id) == id) return ui_.card(i);
    }
    return ui_.card(0);
  }

 private:
  ScriptedClock clock_;
  domain::HostFeed feed_;
  SystemScreen ui_;
  uint32_t sequence_ = 0;
};

// Sweeps one card over many snapshots, checking each rendering fits.
class FitSweep {
 public:
  FitSweep(Overview& overview, const char* id) : overview_(overview), id_(id) {}
  void Check(uint64_t ms, const proto::SnapshotGroups& groups, uint64_t render_ms = 0) {
    overview_.Push(ms, groups);
    overview_.Render(render_ms != 0 ? render_ms : ms);
    Measure();
  }
  void Measure() {
    ++variants_;
    std::string why;
    if (!CardFits(overview_.Card(id_), &why)) {
      std::printf("%s card text does not fit: %s\n", id_, why.c_str());
      fits_ = false;
    }
  }
  int variants() const { return variants_; }
  bool fits() const { return fits_; }

 private:
  Overview& overview_;
  const char* id_;
  int variants_ = 0;
  bool fits_ = true;
};

}  // namespace

// Draws the Overview through the card registry and checks its content, its
// fixed layout, and that every text variant the domain can produce fits.
int RunSystemScreen() {
  ScenarioResult result;
  lv_obj_t* screen = InitLvglHeadless();
  Overview overview(screen);
  SystemScreen& ui = overview.ui();

  domain::ObservationScheduler scheduler;
  domain::CoverageLedger ledger;
  const auto run_windows = [&](uint64_t now_ms, int count, bool lossy) {
    for (int i = 0; i < count; ++i) {
      const auto window = scheduler.NextWindow(now_ms);
      if (!window) break;
      ledger.Record(domain::CompletedReport(*window, lossy ? 0u : window->planned_ms, 0u));
      now_ms += window->planned_ms;
    }
  };
  const auto render = [&](uint64_t now_ms) {
    overview.Render(
        now_ms, domain::MakeCoverageStatus(domain::MakeCoverageView(scheduler, ledger, now_ms)));
  };

  render(1000);
  result.Expect(ui.card_count() == domain::Cards().size(), "one card widget per registered card");
  result.Expect(Has(ui.card(0).title_label(), "CPU") && Has(ui.card(1).title_label(), "RAM"),
                "cards appear in registry order: CPU, RAM");
  result.Expect(ui.card(0).dial() && ui.card(1).dial() && overview.Card("storage").dial() &&
                    !overview.Card("battery").dial() && !overview.Card("network").dial(),
                "CPU, RAM and Storage are dials; Battery and Network are panels");
  result.Expect(Has(ui.card(1).value_label(), "--") && !ui.card(1).dimmed() &&
                    lv_obj_has_flag(ui.card(1).arc(), LV_OBJ_FLAG_HIDDEN),
                "RAM shows -- and no arc before any Host data");

  const proto::RamSnapshot ram{proto::Availability::kAvailable, 8 * kGiB, 16 * kGiB, 0,
                               proto::Freshness::kFresh};
  overview.Push(2000, {.ram = ram});
  render(2000);
  result.Expect(Has(ui.card(0).value_label(), "48%"), "CPU dial shows the live CPU value");
  result.Expect(Has(ui.card(1).value_label(), "50%") &&
                    Has(ui.card(1).note_label(), "8.0 / 16.0 GB") && !ui.card(1).dimmed() &&
                    lv_arc_get_value(ui.card(1).arc()) == 500,
                "RAM dial shows the occupied share, used / total under it, and a live arc");
  result.Expect(Has(ui.connection_label(), LV_SYMBOL_USB),
                "status bar shows live Host data as the USB glyph");

  overview.Push(3000, {.ram = proto::RamSnapshot{proto::Availability::kError, 0, 0, 0,
                                                 proto::Freshness::kFresh}});
  render(3000);
  result.Expect(Has(ui.card(1).value_label(), "--") && Has(ui.card(1).note_label(), "Read error") &&
                    lv_obj_has_flag(ui.card(1).arc(), LV_OBJ_FLAG_HIDDEN),
                "a RAM read error shows -- and its cause, and no arc: never zero");
  result.Expect(Has(ui.card(0).value_label(), "48%"), "CPU is unaffected by the RAM error");

  overview.Push(4000, {.ram = ram});
  render(4000);
  result.Expect(Has(ui.card(1).value_label(), "50%"), "RAM recovers when readable again");

  render(7100);                   // 3.1 s since the last frame
  for (int i = 0; i < 10; ++i) {  // let the decay animation finish
    lv_tick_inc(100);
    lv_timer_handler();
  }
  result.Expect(Has(ui.card(1).value_label(), "Last 50%") && ui.card(1).dimmed() &&
                    lv_arc_get_value(ui.card(1).arc()) == 0 &&
                    !lv_obj_has_flag(ui.card(1).arc(), LV_OBJ_FLAG_HIDDEN),
                "stale RAM keeps its last value labelled Last while the dial decays to neutral");

  // Each radio keeps its own slot in the bar's tray; colour and a mark show
  // its state, and unknown never reads as quiet.
  const auto slot_is = [](lv_obj_t* icon, lv_obj_t* flag, lv_obj_t* slash, uint32_t color,
                          const char* flag_text, bool slashed) {
    // WiFi's icon is a symbol label; BLE's is a drawn rune (a line).
    const lv_color_t icon_color = lv_obj_check_type(icon, &lv_line_class)
                                      ? lv_obj_get_style_line_color(icon, 0)
                                      : lv_obj_get_style_text_color(icon, 0);
    return lv_color_eq(icon_color, theme::Color(color)) &&
           lv_color_eq(lv_obj_get_style_text_color(flag, 0), theme::Color(color)) &&
           std::strcmp(lv_label_get_text(flag), flag_text) == 0 &&
           lv_obj_has_flag(slash, LV_OBJ_FLAG_HIDDEN) != slashed;
  };
  const auto wifi_is = [&](uint32_t color, const char* flag, const char* channel, bool slashed) {
    return slot_is(ui.wifi_icon(), ui.wifi_flag_label(), ui.wifi_slash(), color, flag, slashed) &&
           std::strcmp(lv_label_get_text(ui.wifi_channel_label()), channel) == 0 &&
           lv_color_eq(lv_obj_get_style_text_color(ui.wifi_channel_label(), 0),
                       theme::Color(color));
  };
  const auto ble_is = [&](uint32_t color, const char* flag, bool slashed) {
    return slot_is(ui.ble_icon(), ui.ble_flag_label(), ui.ble_slash(), color, flag, slashed);
  };
  result.Expect(wifi_is(theme::kMuted, "?", "", false) && ble_is(theme::kMuted, "?", false),
                "both radios read unknown (muted, ?), not quiet, before the radio reports");
  run_windows(8000, 2, false);
  render(8500);
  result.Expect(wifi_is(theme::kMuted, "", "ch1", false) && ble_is(theme::kBlue, "", false),
                "the BLE window lights BLE blue; idle WiFi is muted and keeps its channel");
  run_windows(8500, 1, false);
  render(8750);
  result.Expect(wifi_is(theme::kCyan, "", "ch2", false) && ble_is(theme::kMuted, "", false),
                "a WiFi window lights WiFi cyan and names its channel");
  // A window whose receiver is on but has not reported yet is already current.
  overview.Render(8800, domain::MakeCoverageStatus(domain::MakeCoverageView(
                            scheduler.config(), ledger, 8800,
                            domain::ActiveWindow{domain::Radio::kWifi, 3, 0, 8750, 205})));
  result.Expect(wifi_is(theme::kCyan, "", "ch3", false),
                "an active window names its channel before its report arrives");
  run_windows(8705, 2, true);
  render(9200);
  result.Expect(wifi_is(theme::kMuted, "~", "ch3", false) && ble_is(theme::kMuted, "~", false),
                "a lost window shows a muted ~ in its slot");
  scheduler.Configure({.wifi_enabled = false});
  render(9300);
  result.Expect(wifi_is(theme::kDim, "", "", true) && ble_is(theme::kMuted, "~", false),
                "a disabled radio is dim with a slash and no channel");
  scheduler.Configure({.wifi_enabled = false, .ble_enabled = false});
  render(9400);
  result.Expect(wifi_is(theme::kDim, "", "", true) && ble_is(theme::kDim, "", true),
                "Host only is both slots dim and slashed");
  render(200000);
  result.Expect(wifi_is(theme::kDim, "", "", true) && ble_is(theme::kDim, "", true) &&
                    !Has(ui.connection_label(), "Connected"),
                "the tray stays in the bar while the Host link goes stale");

  // Every slot state for both radios: its colour, mark, slash and channel, and
  // nothing in the bar moves when the state changes.
  {
    using domain::RadioSlotState;
    struct Expected {
      RadioSlotState state;
      uint32_t wifi_color;
      uint32_t ble_color;
      const char* flag;
      bool slashed;
    };
    const Expected expected[] = {
        {RadioSlotState::kScanning, theme::kCyan, theme::kBlue, "", false},
        {RadioSlotState::kIdle, theme::kMuted, theme::kMuted, "", false},
        {RadioSlotState::kOff, theme::kDim, theme::kDim, "", true},
        {RadioSlotState::kGap, theme::kMuted, theme::kMuted, "~", false},
        {RadioSlotState::kNoData, theme::kMuted, theme::kMuted, "?", false},
        {RadioSlotState::kFault, theme::kText, theme::kText, "!", false},
    };
    lv_obj_t* const parts[] = {ui.connection_label(),   ui.radio_tray(),      ui.wifi_icon(),
                               ui.wifi_channel_label(), ui.wifi_flag_label(), ui.ble_icon(),
                               ui.ble_flag_label(),     ui.tray_separator(),  ui.clock_label()};
    const auto coords = [&] {
      std::vector<lv_area_t> areas;
      lv_obj_update_layout(screen);
      for (lv_obj_t* part : parts) {
        lv_area_t area;
        lv_obj_get_coords(part, &area);
        areas.push_back(area);
      }
      return areas;
    };
    const auto same = [](const std::vector<lv_area_t>& a, const std::vector<lv_area_t>& b) {
      for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].x1 != b[i].x1 || a[i].x2 != b[i].x2 || a[i].y1 != b[i].y1 || a[i].y2 != b[i].y2)
          return false;
      }
      return true;
    };
    overview.Render(300000, {});
    const auto reference = coords();
    bool fixed = true, all_right = true;
    for (const Expected& e : expected) {
      for (const uint8_t channel : {uint8_t{0}, uint8_t{6}, uint8_t{11}, uint8_t{14}}) {
        domain::CoverageStatus status;
        status.wifi = {e.state, channel};
        status.ble = {e.state, 0};
        overview.Render(300000, status);
        const std::string text = channel == 0 ? "" : "ch" + std::to_string(channel);
        all_right = all_right && wifi_is(e.wifi_color, e.flag, text.c_str(), e.slashed) &&
                    ble_is(e.ble_color, e.flag, e.slashed);
        fixed = fixed && same(reference, coords());
      }
    }
    result.Expect(all_right, "every slot state shows its colour, mark, slash and channel");
    result.Expect(fixed, "no tray, connection or clock position moves with state or channel");
  }

  // The persistent bar: connection, a free centre, the tray and the clock.
  // The tray is fixed-width, so its flags count in the worst case; the longest
  // connection and clock must fit with it.
  {
    lv_obj_update_layout(screen);
    const auto widest = [&](lv_obj_t* label, const std::vector<std::string>& texts,
                            std::string* which) {
      int best = 0;
      for (const auto& text : texts) {
        if (TextWidth(label, text) > best) {
          best = TextWidth(label, text);
          *which = text;
        }
      }
      return best;
    };
    // Every connection text MakeCpuView writes, and the longest clocks: 12 h
    // with an unknown zone (so it reads UTC), and no time at all.
    std::vector<std::string> connections;
    for (const auto connection :
         {domain::ConnectionState::kNegotiating, domain::ConnectionState::kLive,
          domain::ConnectionState::kStale, domain::ConnectionState::kDisconnected,
          domain::ConnectionState::kIncompatible}) {
      domain::HostState state;
      state.connection = connection;
      connections.push_back(domain::MakeCpuView(state, 0).connection);
    }
    const std::vector<std::string> clocks = {"Time unavailable", "12:58 PM UTC", "12:58 PM",
                                             "23:58 UTC", "23:58"};
    std::string long_connection, long_clock;
    const int connection_w = widest(ui.connection_label(), connections, &long_connection);
    const int clock_w = widest(ui.clock_label(), clocks, &long_clock);
    // Show the flags on both radios and the widest channel, then measure.
    domain::CoverageStatus worst;
    worst.wifi = {domain::RadioSlotState::kFault, 14};
    worst.ble = {domain::RadioSlotState::kNoData, 0};
    overview.Render(300000, worst);
    lv_obj_update_layout(screen);
    const int tray_w = lv_obj_get_width(ui.radio_tray());
    const int bar_w = lv_obj_get_content_width(ui.status_bar());
    const int need_worst = connection_w + tray_w + clock_w;
    std::printf(
        "status bar %d px: worst connection \"%s\" %d + tray (flags shown) %d + clock \"%s\" %d "
        "= %d\n",
        bar_w, long_connection.c_str(), connection_w, tray_w, long_clock.c_str(), clock_w,
        need_worst);
    result.Expect(need_worst <= bar_w,
                  "the longest connection and clock fit the bar together with the tray");
    // Each fixed box holds its widest text.
    bool boxes_fit = true;
    for (const char* text : {"ch11", "ch14", "ch10", "ch13", "ch12"})
      boxes_fit = boxes_fit && TextWidth(ui.wifi_channel_label(), text) <= 27;
    for (const char* text : {"~", "?", "!"})
      boxes_fit = boxes_fit && TextWidth(ui.wifi_flag_label(), text) <= 7;
    result.Expect(boxes_fit, "the channel box fits ch14 and the flag box fits ~ ? !");
    lv_area_t tray_box, connection_box, clock_box;
    lv_obj_get_coords(ui.radio_tray(), &tray_box);
    lv_obj_get_coords(ui.connection_label(), &connection_box);
    lv_obj_get_coords(ui.clock_label(), &clock_box);
    result.Expect(tray_box.x1 > connection_box.x2 && tray_box.x2 < clock_box.x1,
                  "the tray sits between the connection and the clock");
    result.Expect(
        lv_obj_get_height(ui.tray_separator()) == 12 && lv_obj_get_width(ui.tray_separator()) == 1,
        "the separator is a 1x12 hairline");
  }

  // The fixed layout: every card and the Agents bar sit inside the 320x240
  // display without overlapping, and nothing scrolls.
  {
    lv_obj_update_layout(screen);
    std::vector<lv_area_t> boxes;
    for (size_t i = 0; i < ui.card_count(); ++i) {
      lv_area_t box;
      lv_obj_get_coords(ui.card(i).root(), &box);
      boxes.push_back(box);
    }
    lv_area_t bar_box, agents_box;
    lv_obj_get_coords(ui.status_bar(), &bar_box);
    lv_obj_get_coords(ui.agents_bar().root(), &agents_box);
    boxes.push_back(bar_box);
    boxes.push_back(agents_box);
    bool inside = true, apart = true;
    for (size_t i = 0; i < boxes.size(); ++i) {
      const lv_area_t& a = boxes[i];
      inside = inside && a.x1 >= 0 && a.x2 < 320 && a.y1 >= 0 && a.y2 < 240;
      for (size_t j = i + 1; j < boxes.size(); ++j) {
        const lv_area_t& b = boxes[j];
        apart = apart && (a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
      }
    }
    result.Expect(inside, "the status bar, every card and the Agents bar sit inside 320x240");
    result.Expect(apart, "no two Overview parts overlap");
    result.Expect(agents_box.y1 > bar_box.y2 && agents_box.y2 >= 230,
                  "the Agents bar runs along the bottom");
    result.Expect(!lv_obj_has_flag(ui.root(), LV_OBJ_FLAG_SCROLLABLE) &&
                      lv_obj_get_scroll_bottom(ui.root()) <= 0,
                  "the Overview does not scroll");
  }

  // CPU: every whole percentage, live and Last.
  {
    FitSweep sweep(overview, "cpu");
    for (uint16_t per_mille = 0; per_mille <= 1000; per_mille += 5) {
      overview.Push(100'000, {}, per_mille);
      overview.Render(100'000);
      sweep.Measure();
    }
    overview.Render(104'000);
    sweep.Measure();
    result.Expect(sweep.fits(), "every CPU dial text fits (fresh and Last)");
  }

  // RAM: shares and sizes up to 512 GB, live, Last, and each unavailable wording.
  {
    FitSweep sweep(overview, "ram");
    for (const uint64_t total : {8 * kGiB, 16 * kGiB, 128 * kGiB, 192 * kGiB, 512 * kGiB}) {
      for (const uint64_t used : {uint64_t{0}, total / 3, total - kGiB / 10, total}) {
        const proto::RamSnapshot g{proto::Availability::kAvailable, used, total, 0,
                                   proto::Freshness::kFresh};
        sweep.Check(200'000, {.ram = g});
        sweep.Check(200'000, {.ram = g}, 204'000);
      }
    }
    for (uint8_t availability = 1; availability <= 4; ++availability) {
      sweep.Check(210'000,
                  {.ram = proto::RamSnapshot{static_cast<proto::Availability>(availability), 0, 0,
                                             0, proto::Freshness::kFresh}});
    }
    result.Expect(sweep.variants() == 44 && sweep.fits(),
                  "every RAM dial text fits (fresh, Last and unavailable)");
  }

  // Storage: capacity and I/O fresh, stale or unavailable in every
  // combination, with the widest rates.
  {
    FitSweep sweep(overview, "storage");
    const proto::Availability availabilities[] = {
        proto::Availability::kAvailable, proto::Availability::kUnsupported,
        proto::Availability::kAbsent, proto::Availability::kTemporarilyUnavailable,
        proto::Availability::kError};
    for (const auto capacity : availabilities) {
      for (const auto io : availabilities) {
        for (const uint64_t rate : {uint64_t{0}, uint64_t{999}, uint64_t{9'990'000}, UINT64_MAX}) {
          for (const uint32_t capacity_age : {0u, 100'000u}) {
            for (const uint32_t io_age : {0u, 4'000u}) {
              const bool cap_ok = capacity == proto::Availability::kAvailable;
              const bool io_ok = io == proto::Availability::kAvailable;
              const proto::StorageSnapshot g{
                  capacity, cap_ok ? 994 * kGB : 0, cap_ok ? 5 * kGB : 0, capacity_age,
                  io,       io_ok ? rate : 0,       io_ok ? rate : 0,     io_age};
              sweep.Check(300'000, {.storage = g});
            }
          }
        }
      }
    }
    overview.Render(340'000);  // link lost: everything retained
    sweep.Measure();
    result.Expect(sweep.variants() > 400 && sweep.fits(),
                  "every Storage dial text fits (fresh, Last and unavailable)");
  }

  // Battery: every state and condition wire value, fresh and Last.
  {
    FitSweep sweep(overview, "battery");
    for (const uint32_t age : {0u, 200'000u}) {
      for (uint8_t state = 0; state <= 4; ++state) {
        for (uint8_t condition = 0; condition <= 3; ++condition) {
          for (const uint16_t per_mille : {uint16_t{1000}, uint16_t{870}, uint16_t{5}}) {
            proto::BatterySnapshot g;
            g.charge_availability = proto::Availability::kAvailable;
            g.charge_per_mille = per_mille;
            g.state = static_cast<proto::BatteryState>(state);
            g.charge_age_ms = age;
            g.condition_availability = proto::Availability::kAvailable;
            g.max_capacity_per_mille = per_mille;
            g.condition = static_cast<proto::BatteryCondition>(condition);
            g.condition_age_ms = age * 100;
            sweep.Check(1'000'000, {.battery = g});
          }
        }
      }
    }
    result.Expect(sweep.variants() == 120 && sweep.fits(),
                  "every Battery value and note variant fits (fresh and Last)");
  }

  // Network: each route kind (tunnel or not), coverage and channel up to the
  // wire maximum of 255, fresh and stale halves, the widest rates, and each
  // unavailable wording.
  {
    FitSweep sweep(overview, "network");
    const proto::RouteKind kinds[] = {proto::RouteKind::kUnknown, proto::RouteKind::kWifi,
                                      proto::RouteKind::kEthernet, proto::RouteKind::kOther,
                                      proto::RouteKind::kNone};
    const proto::WifiCoverage coverages[] = {proto::WifiCoverage::kUnknown,
                                             proto::WifiCoverage::kObservable,
                                             proto::WifiCoverage::kOutsideSupported};
    using Links = std::optional<std::vector<proto::RouteKind>>;
    const Links link_sets[] = {
        std::nullopt, std::vector<proto::RouteKind>{proto::RouteKind::kWifi},
        std::vector<proto::RouteKind>{proto::RouteKind::kWifi, proto::RouteKind::kEthernet},
        std::vector<proto::RouteKind>{proto::RouteKind::kWifi, proto::RouteKind::kEthernet,
                                      proto::RouteKind::kOther, proto::RouteKind::kOther,
                                      proto::RouteKind::kEthernet}};
    for (const Links& links : link_sets)
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
                    g.links = links;
                    sweep.Check(3'000'000, {.network = g});
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
      sweep.Check(3'000'000, {.network = g});
    }
    // Offline retained after the Host link is lost: "Last Offline", dimmed.
    proto::NetworkSnapshot offline;
    offline.traffic_availability = proto::Availability::kAbsent;
    offline.route_kind = proto::RouteKind::kNone;
    sweep.Check(3'000'000, {.network = offline}, 3'040'000);
    result.Expect(Has(overview.Card("network").note_label(), "Last Offline") &&
                      overview.Card("network").dimmed(),
                  "offline after link loss reads Last Offline, dimmed");
    result.Expect(sweep.variants() > 500 && sweep.fits(),
                  "every Network value and note variant fits (fresh and Last)");
  }

  // The Agents bar: every combination of the five agents' wire states, fresh
  // and Last, plus each unavailable wording, fits the bar; and each state has
  // its own look, whose animation stops when the reading is retained.
  {
    const firmware::ui::system::AgentsBar& bar = ui.agents_bar();
    int variants = 0;
    bool fits = true;
    const auto measure = [&] {
      ++variants;
      std::string why;
      if (!ChildrenInside(bar.root(), &why)) {
        std::string shown = lv_label_get_text(bar.message_label());
        for (size_t slot = 0; slot < proto::kAgentSlots; ++slot) {
          if (bar.lit(slot)) shown += " [slot " + std::to_string(slot) + "]";
        }
        std::printf("agents bar does not fit: %s (%s)\n", why.c_str(), shown.c_str());
        fits = false;
      }
    };
    for (const uint32_t age : {0u, 20'000u}) {
      // 4^5 combinations: one base-4 digit per wire slot.
      for (unsigned combination = 0; combination < 1024; ++combination) {
        proto::AgentsSnapshot g;
        g.availability = proto::Availability::kAvailable;
        g.source_age_ms = age;
        for (size_t slot = 0; slot < proto::kAgentSlots; ++slot) {
          g.state[slot] = static_cast<proto::AgentState>((combination >> (2 * slot)) & 3);
        }
        overview.Push(2'000'000, {.agents = g});
        overview.Render(2'000'000);
        measure();
      }
    }
    for (uint8_t availability = 1; availability <= 4; ++availability) {
      proto::AgentsSnapshot g;
      g.availability = static_cast<proto::Availability>(availability);
      overview.Push(2'000'000, {.agents = g});
      overview.Render(2'000'000);
      measure();
      bool any_visible = false;
      for (size_t slot = 0; slot < proto::kAgentSlots; ++slot) {
        any_visible = any_visible || bar.mascot_visible(slot);
      }
      result.Expect(Has(bar.message_label(), "--") && !any_visible,
                    "an unavailable process listing is -- with its cause and no mascots, never "
                    "silhouettes that read as nothing running");
    }
    result.Expect(variants == 2052 && fits, "every Agents bar variant fits (fresh and Last)");

    proto::AgentsSnapshot mixed;
    mixed.availability = proto::Availability::kAvailable;
    mixed.state = {proto::AgentState::kBusy, proto::AgentState::kRunning, proto::AgentState::kIdle,
                   proto::AgentState::kBusy, proto::AgentState::kRunning};
    overview.Push(2'100'000, {.agents = mixed});
    overview.Render(2'100'000);
    result.Expect(bar.lit(0) && bar.animating(0), "a busy agent's mascot is lit and animated");
    result.Expect(bar.lit(1) && !bar.animating(1),
                  "an agent of unknown activity is lit and still, never shown busy");
    result.Expect(bar.lit(2) && !bar.animating(2), "an idle agent is lit and still");
    result.Expect(bar.lit(3) && bar.animating(3), "a busy Gemini sparkle is lit and twinkling");
    result.Expect(bar.lit(4) && !bar.animating(4),
                  "an OpenCode of unknown activity is lit and still");
    {
      // Left to right: Codex, Claude, Gemini (wire slot 3), Grok (2), OpenCode (4).
      lv_obj_update_layout(bar.root());
      const auto centre = [&](size_t slot) {
        return lv_obj_get_x(bar.mascot(slot)) + lv_obj_get_width(bar.mascot(slot)) / 2;
      };
      result.Expect(centre(0) < centre(1) && centre(1) < centre(3) && centre(3) < centre(2) &&
                        centre(2) < centre(4),
                    "the mascots sit left to right Codex, Claude, Gemini, Grok, OpenCode");
    }
    overview.Render(2'104'000);  // 4 s without a frame: link stale
    result.Expect(bar.dimmed() && Has(bar.message_label(), "Last") && bar.lit(0) && bar.lit(1) &&
                      bar.lit(3) && !bar.animating(0) && !bar.animating(3),
                  "a retained reading keeps its mascots lit, labelled Last, every one still");
    proto::AgentsSnapshot none;
    none.availability = proto::Availability::kAvailable;
    overview.Push(2'110'000, {.agents = none});
    overview.Render(2'110'000);
    result.Expect(lv_obj_has_flag(bar.message_label(), LV_OBJ_FLAG_HIDDEN) &&
                      bar.mascot_visible(0) && bar.mascot_visible(1) && bar.mascot_visible(2) &&
                      bar.mascot_visible(3) && bar.mascot_visible(4) && !bar.lit(0) &&
                      !bar.lit(1) && !bar.lit(2) && !bar.lit(3) && !bar.lit(4) && !bar.dimmed(),
                  "with nothing running every mascot is a dim silhouette, with no words");
    overview.Render(2'110'000, {}, "Incompatible protocol: Host data rejected");
    std::string why;
    result.Expect(!lv_obj_has_flag(bar.notice_label(), LV_OBJ_FLAG_HIDDEN) &&
                      lv_obj_has_flag(bar.message_label(), LV_OBJ_FLAG_HIDDEN) &&
                      !bar.mascot_visible(0) && ChildrenInside(bar.root(), &why),
                  "a link fault takes the Agents bar's place and fits it");
  }
  return result.Finish("simulator system-screen");
}

}  // namespace simulator::scenarios
