// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/card_registry.hpp"

namespace firmware::ui::system {

// One registered metric card, in one of two layouts chosen by the view's
// `dial` flag: a dial (arc with the value in its centre and one note line
// under it) or a panel (title, value, note, and a bar along the bottom). It
// renders a MetricCardView and owns no state. A dimmed card fades to half
// opacity and its arc or bar decays to empty, while the value text keeps the
// last reading labelled "Last" (docs/ui-design.md, Stale host state).
class MetricCard {
 public:
  MetricCard(lv_obj_t* parent, int32_t width, int32_t height, bool dial, lv_color_t accent);
  ~MetricCard();
  MetricCard(const MetricCard&) = delete;
  MetricCard& operator=(const MetricCard&) = delete;

  void Update(const firmware::domain::MetricCardView& view);

  bool dial() const { return arc_ != nullptr; }
  bool dimmed() const { return dimmed_; }
  lv_obj_t* root() const { return root_; }
  lv_obj_t* title_label() const { return title_label_; }
  lv_obj_t* value_label() const { return value_label_; }
  lv_obj_t* note_label() const { return note_label_; }
  // The bar (panel layout) or the arc (dial layout); the other is null.
  lv_obj_t* bar() const { return bar_; }
  lv_obj_t* arc() const { return arc_; }

 private:
  void BuildDial(lv_color_t accent);
  void BuildPanel(lv_color_t accent);

  lv_obj_t* root_ = nullptr;
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* value_label_ = nullptr;
  lv_obj_t* note_label_ = nullptr;
  lv_obj_t* bar_ = nullptr;
  lv_obj_t* arc_ = nullptr;
  bool dimmed_ = false;
};

}  // namespace firmware::ui::system
