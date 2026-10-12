// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/card_registry.hpp"

namespace firmware::ui::system {

// One registered metric card, in the layout its view names (CardLayout):
//   kDial     an arc with the value in its centre and, under it, one note line
//             or two fixed rate slots (a coloured letter and a value each);
//   kPanel    title, value, note, and a bar along the bottom;
//   kBattery  a battery cell filled to the charge (a bolt while on power), the
//             value and detail beside it, and a meter line underneath;
//   kRates    a link line (one icon per active link, then the note) over two
//             rate rows, each an arrow, a value and a log-scale meter.
// It renders a MetricCardView and owns no state. A dimmed card fades to half
// opacity and its arc or bars decay to empty (the battery cell turns grey
// instead: an empty cell would read as a flat battery), while the text keeps
// the last reading labelled "Last" (docs/ui-design.md, Stale host state).
class MetricCard {
 public:
  using Layout = firmware::domain::CardLayout;

  MetricCard(lv_obj_t* parent, int32_t width, int32_t height, Layout layout, lv_color_t accent);
  ~MetricCard();
  MetricCard(const MetricCard&) = delete;
  MetricCard& operator=(const MetricCard&) = delete;

  void Update(const firmware::domain::MetricCardView& view);

  Layout layout() const { return layout_; }
  bool dial() const { return layout_ == Layout::kDial; }
  bool dimmed() const { return dimmed_; }
  lv_obj_t* root() const { return root_; }
  // Null for the battery and rates layouts, whose glyphs name them.
  lv_obj_t* title_label() const { return title_label_; }
  // The percentage (dial, panel, battery) or the first rate (rates).
  lv_obj_t* value_label() const { return value_label_; }
  // The line under the value: a dial's note, a panel's note, the battery's
  // detail or cause, the rates layout's link line.
  lv_obj_t* note_label() const { return note_label_; }
  // The bar (panel), the cell fill (battery), or the arc (dial); else null.
  lv_obj_t* bar() const { return bar_; }
  lv_obj_t* arc() const { return arc_; }
  // Battery: the meter line's caption, bar and value, and the bolt.
  lv_obj_t* meter_caption() const { return meter_caption_; }
  lv_obj_t* meter() const { return meter_; }
  lv_obj_t* meter_value() const { return meter_value_; }
  lv_obj_t* bolt() const { return bolt_; }
  // Dial rate slots and rates rows: index 0 is the first rate, 1 the second.
  lv_obj_t* rate_value(int index) const { return rate_value_[index]; }
  lv_obj_t* rate_meter(int index) const { return rate_meter_[index]; }
  // The link line's icon slots (rates layout): the WiFi glyph or a pixel
  // icon, and which one each shows; hidden slots show nothing.
  lv_obj_t* link_wifi(size_t slot) const { return link_wifi_[slot]; }
  lv_obj_t* link_image(size_t slot) const { return link_image_[slot]; }

 private:
  void BuildDial(lv_color_t accent);
  void BuildPanel(lv_color_t accent);
  void BuildBattery(lv_color_t accent);
  void BuildRates();
  void UpdateDial(const firmware::domain::MetricCardView& view, bool dim_changed);
  void UpdateBattery(const firmware::domain::MetricCardView& view, bool dim_changed);
  void UpdateRates(const firmware::domain::MetricCardView& view, bool dim_changed);

  Layout layout_;
  lv_color_t accent_;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* value_label_ = nullptr;
  lv_obj_t* note_label_ = nullptr;
  lv_obj_t* bar_ = nullptr;
  lv_obj_t* arc_ = nullptr;
  lv_obj_t* meter_caption_ = nullptr;
  lv_obj_t* meter_ = nullptr;
  lv_obj_t* meter_value_ = nullptr;
  lv_obj_t* bolt_ = nullptr;
  lv_obj_t* cell_ = nullptr;
  lv_obj_t* rate_slots_ = nullptr;  // dial: the container of the two slots
  lv_obj_t* rate_value_[2] = {};
  lv_obj_t* rate_meter_[2] = {};
  lv_obj_t* link_wifi_[firmware::domain::kMaxLinkGlyphs] = {};
  lv_obj_t* link_image_[firmware::domain::kMaxLinkGlyphs] = {};
  bool dimmed_ = false;
};

}  // namespace firmware::ui::system
