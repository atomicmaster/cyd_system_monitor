// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/card_registry.hpp"

namespace firmware::ui::system {

// One registered metric card: title, value, availability note, and a bar. It
// renders a MetricCardView and owns no state; a card that is dimmed fades to
// half opacity (a retained reading, never an animated fall toward zero).
class MetricCard {
 public:
  MetricCard(lv_obj_t* parent, int32_t width, int32_t height);
  ~MetricCard();
  MetricCard(const MetricCard&) = delete;
  MetricCard& operator=(const MetricCard&) = delete;

  void Update(const firmware::domain::MetricCardView& view);

  bool dimmed() const { return dimmed_; }
  lv_obj_t* title_label() const { return title_label_; }
  lv_obj_t* value_label() const { return value_label_; }
  lv_obj_t* note_label() const { return note_label_; }
  lv_obj_t* bar() const { return bar_; }

 private:
  lv_obj_t* root_ = nullptr;
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* value_label_ = nullptr;
  lv_obj_t* note_label_ = nullptr;
  lv_obj_t* bar_ = nullptr;
  bool dimmed_ = false;
};

}  // namespace firmware::ui::system
