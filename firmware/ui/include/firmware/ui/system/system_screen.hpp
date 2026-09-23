// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/ui/system/card/metric_card.hpp"

namespace firmware::ui::system {

// The System shell: a persistent status bar (connection and clock) above the
// registered metric cards, two per row. Cards are created to match the view
// list, so a feature ticket that registers a card needs no change here.
class SystemScreen {
 public:
  explicit SystemScreen(lv_obj_t* parent);
  ~SystemScreen();
  SystemScreen(const SystemScreen&) = delete;
  SystemScreen& operator=(const SystemScreen&) = delete;

  // `status` supplies the bar text; `cards` are in registry order. `coverage`
  // is the radio coverage text shown in the bar (empty hides it).
  void Update(const firmware::domain::CpuView& status,
              std::span<const firmware::domain::MetricCardView> cards,
              const std::string& coverage = {});

  // Tapping the status bar (connection and clock) reports here; the owner
  // decides what it opens.
  void SetStatusBarTapCallback(std::function<void()> callback) {
    on_status_tap_ = std::move(callback);
  }
  lv_obj_t* status_bar() const { return bar_row_; }
  lv_obj_t* connection_label() const { return connection_label_; }
  lv_obj_t* clock_label() const { return clock_label_; }
  lv_obj_t* coverage_label() const { return coverage_label_; }
  size_t card_count() const { return cards_.size(); }
  const MetricCard& card(size_t index) const { return *cards_[index]; }

 private:
  lv_obj_t* root_ = nullptr;
  std::function<void()> on_status_tap_;
  lv_obj_t* bar_row_ = nullptr;
  lv_obj_t* connection_label_ = nullptr;
  lv_obj_t* coverage_label_ = nullptr;
  lv_obj_t* clock_label_ = nullptr;
  lv_obj_t* card_area_ = nullptr;
  std::vector<std::unique_ptr<MetricCard>> cards_;
};

}  // namespace firmware::ui::system
