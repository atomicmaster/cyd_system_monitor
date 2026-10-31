// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "firmware/domain/agents_bar.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/coverage.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/ui/system/agents/agents_bar.hpp"
#include "firmware/ui/system/card/metric_card.hpp"

namespace firmware::ui::system {

// The Overview on the 320x240 display, top to bottom:
//   a persistent status bar: the connection on the left, a centre kept free
//     for the future Alert, then a tray of one fixed slot per radio (icon,
//     colour for its state, WiFi channel) beside the clock;
//   the registered cards: dial cards three to a row, then panel cards two to
//     a row, sized so the registry's cards fill the area with no scrolling;
//   the Agents bar.
// Cards are created to match the view list, so a feature ticket that
// registers a card needs no change here.
class SystemScreen {
 public:
  explicit SystemScreen(lv_obj_t* parent);
  ~SystemScreen();
  SystemScreen(const SystemScreen&) = delete;
  SystemScreen& operator=(const SystemScreen&) = delete;

  // `status` supplies the bar text; `cards` are in registry order. `coverage`
  // is each radio's slot state for the tray (the default reads unknown for
  // both). A non-empty `notice` (a link fault) takes the Agents bar's place.
  // Call it every draw: widgets are touched only when their value changed.
  void Update(const firmware::domain::CpuView& status,
              std::span<const firmware::domain::MetricCardView> cards,
              const firmware::domain::AgentsBarView& agents = {},
              const firmware::domain::CoverageStatus& coverage = {},
              const std::string& notice = {});

  // Tapping the connection text reports to the first callback (host link
  // details); tapping anywhere else on the bar reports to the second. The
  // owner decides what they open.
  void SetConnectionTapCallback(std::function<void()> callback) {
    on_connection_tap_ = std::move(callback);
  }
  void SetStatusBarTapCallback(std::function<void()> callback) {
    on_status_tap_ = std::move(callback);
  }
  lv_obj_t* root() const { return root_; }
  lv_obj_t* status_bar() const { return bar_row_; }
  lv_obj_t* connection_label() const { return connection_label_; }
  lv_obj_t* clock_label() const { return clock_label_; }
  // The radio tray and its parts, for tests. A slash is visible only for an
  // operator-disabled radio.
  lv_obj_t* radio_tray() const { return tray_; }
  lv_obj_t* wifi_icon() const { return wifi_.icon; }
  lv_obj_t* wifi_channel_label() const { return wifi_channel_; }
  lv_obj_t* wifi_flag_label() const { return wifi_.flag; }
  lv_obj_t* wifi_slash() const { return wifi_.slash; }
  lv_obj_t* ble_icon() const { return ble_.icon; }
  lv_obj_t* ble_flag_label() const { return ble_.flag; }
  lv_obj_t* ble_slash() const { return ble_.slash; }
  lv_obj_t* tray_separator() const { return separator_; }
  size_t card_count() const { return cards_.size(); }
  const MetricCard& card(size_t index) const { return *cards_[index]; }
  const AgentsBar& agents_bar() const { return *agents_bar_; }

 private:
  lv_obj_t* root_ = nullptr;
  std::function<void()> on_connection_tap_;
  std::function<void()> on_status_tap_;
  // One radio's icon (a symbol label for WiFi, a drawn rune for BLE), the
  // slash drawn across it when off, and its flag.
  struct RadioWidgets {
    lv_obj_t* icon = nullptr;
    lv_obj_t* slash_halo = nullptr;
    lv_obj_t* slash = nullptr;
    lv_obj_t* flag = nullptr;
    std::array<lv_point_precise_t, 2> slash_points{};  // the lines keep a pointer to these
    std::array<lv_point_precise_t, 6> rune_points{};
  };
  // Builds one radio's icon slot (and its slash) in `group`. A null `symbol`
  // draws the Bluetooth rune: the font's glyph is a filled capsule at 12 pt.
  void BuildRadioIcon(RadioWidgets& widgets, lv_obj_t* group, const char* symbol);
  void ApplySlot(const RadioWidgets& widgets, const firmware::domain::RadioSlotView& slot,
                 uint32_t accent);
  lv_obj_t* bar_row_ = nullptr;
  lv_obj_t* connection_label_ = nullptr;
  lv_obj_t* tray_ = nullptr;
  RadioWidgets wifi_;
  RadioWidgets ble_;
  lv_obj_t* wifi_channel_ = nullptr;
  lv_obj_t* separator_ = nullptr;
  // What the tray shows now, so an unchanged value touches nothing.
  std::optional<firmware::domain::CoverageStatus> shown_coverage_;
  lv_obj_t* clock_label_ = nullptr;
  lv_obj_t* card_area_ = nullptr;
  std::vector<std::unique_ptr<MetricCard>> cards_;
  std::unique_ptr<AgentsBar> agents_bar_;
};

}  // namespace firmware::ui::system
