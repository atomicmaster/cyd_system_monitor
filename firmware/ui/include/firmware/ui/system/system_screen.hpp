// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "firmware/domain/agents_bar.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/ui/system/agents/agents_bar.hpp"
#include "firmware/ui/system/card/metric_card.hpp"

namespace firmware::ui::system {

// The Overview on the 320x240 display, top to bottom:
//   a persistent status bar (connection, current Observation Window, clock);
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
  // is the radio coverage text shown in the bar (empty hides it). A non-empty
  // `notice` (a link fault) takes the Agents bar's place.
  void Update(const firmware::domain::CpuView& status,
              std::span<const firmware::domain::MetricCardView> cards,
              const firmware::domain::AgentsBarView& agents = {}, const std::string& coverage = {},
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
  lv_obj_t* coverage_label() const { return coverage_label_; }
  size_t card_count() const { return cards_.size(); }
  const MetricCard& card(size_t index) const { return *cards_[index]; }
  const AgentsBar& agents_bar() const { return *agents_bar_; }

 private:
  lv_obj_t* root_ = nullptr;
  std::function<void()> on_connection_tap_;
  std::function<void()> on_status_tap_;
  lv_obj_t* bar_row_ = nullptr;
  lv_obj_t* connection_label_ = nullptr;
  lv_obj_t* coverage_label_ = nullptr;
  lv_obj_t* clock_label_ = nullptr;
  lv_obj_t* card_area_ = nullptr;
  std::vector<std::unique_ptr<MetricCard>> cards_;
  std::unique_ptr<AgentsBar> agents_bar_;
};

}  // namespace firmware::ui::system
