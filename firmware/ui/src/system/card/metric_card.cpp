// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/card/metric_card.hpp"

#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {
namespace {

using firmware::domain::CardLayout;

constexpr uint32_t kDimAnimationMs = 300;
constexpr uint32_t kDecayAnimationMs = 800;
constexpr int32_t kDialSize = 64;
constexpr int32_t kDialTop = 13;
constexpr int32_t kDialWidth = 6;
// Battery and rates panels: a 146x66 content area (154x74 card, 3 px padding,
// 1 px border).
constexpr int32_t kTextLeft = 56;  // battery value and detail, right of the cell
constexpr int32_t kRowValueX = 18;
constexpr int32_t kRowMeterX = 70;

void SetOpacity(void* object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

void SetArcValue(void* arc, int32_t value) { lv_arc_set_value(static_cast<lv_obj_t*>(arc), value); }

void Animate(lv_obj_t* object, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
             uint32_t duration_ms) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_exec_cb(&animation, exec);
  lv_anim_set_values(&animation, from, to);
  lv_anim_set_duration(&animation, duration_ms);
  lv_anim_start(&animation);
}

void SetHidden(lv_obj_t* object, bool hidden) {
  if (hidden) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  }
}

// Fixed-size lines: text that is too long is clipped, never wrapped, so a card
// cannot grow past its grid cell.
lv_obj_t* Line(lv_obj_t* parent) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  return label;
}

// A clipped label at a fixed place; `width` 0 sizes it to its text.
lv_obj_t* Placed(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int32_t x, int32_t y,
                 int32_t width = 0) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, theme::Color(color), 0);
  lv_obj_set_pos(label, x, y);
  if (width > 0) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  }
  lv_label_set_text(label, "");
  return label;
}

// A thin horizontal meter over a track, 0-1000.
lv_obj_t* Meter(lv_obj_t* parent, int32_t x, int32_t y, int32_t width, int32_t height,
                lv_color_t color) {
  lv_obj_t* bar = lv_bar_create(parent);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(bar, x, y);
  lv_obj_set_size(bar, width, height);
  lv_bar_set_range(bar, 0, 1000);
  lv_obj_set_style_bg_color(bar, theme::Color(theme::kTrack), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
  lv_obj_set_style_anim_duration(bar, kDecayAnimationMs, 0);
  return bar;
}

// A box with no theme styling (cell outline, nub, containers).
lv_obj_t* Box(lv_obj_t* parent) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

void SetMeter(lv_obj_t* meter, int32_t per_mille, bool decay) {
  lv_bar_set_value(meter, per_mille, decay ? LV_ANIM_ON : LV_ANIM_OFF);
}

}  // namespace

MetricCard::MetricCard(lv_obj_t* parent, int32_t width, int32_t height, Layout layout,
                       lv_color_t accent)
    : layout_(layout), accent_(accent) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, width, height);
  theme::StylePanel(root_);
  lv_obj_set_style_pad_all(root_, 3, 0);
  switch (layout_) {
    case CardLayout::kDial:
      BuildDial(accent);
      break;
    case CardLayout::kPanel:
      BuildPanel(accent);
      break;
    case CardLayout::kBattery:
      BuildBattery(accent);
      break;
    case CardLayout::kRates:
      BuildRates();
      break;
  }
  Update(firmware::domain::MetricCardView{.title = "", .value = "--", .layout = layout_});
}

void MetricCard::BuildDial(lv_color_t accent) {
  title_label_ = Line(root_);
  theme::StyleCaption(title_label_);
  // A 270-degree arc open at the bottom, like a gauge; the value sits in its
  // centre and the note (or the rate slots) in the opening below.
  lv_obj_t* holder = Box(root_);
  lv_obj_set_size(holder, kDialSize, kDialSize);
  lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, kDialTop);
  arc_ = lv_arc_create(holder);
  lv_obj_set_size(arc_, kDialSize, kDialSize);
  lv_obj_center(arc_);
  lv_arc_set_rotation(arc_, 135);
  lv_arc_set_bg_angles(arc_, 0, 270);
  lv_arc_set_range(arc_, 0, 1000);
  lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc_, kDialWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_, kDialWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, theme::Color(theme::kTrack), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc_, accent, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(arc_, 0, 0);

  value_label_ = lv_label_create(holder);
  lv_obj_set_width(value_label_, kDialSize - 2 * kDialWidth - 4);
  // "Last 62%" wraps onto two lines inside the ring rather than clipping.
  lv_label_set_long_mode(value_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(value_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(value_label_, accent, 0);
  lv_obj_center(value_label_);

  note_label_ = Line(root_);
  theme::StyleCaption(note_label_);
  lv_obj_set_style_text_align(note_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(note_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

  // Two fixed slots, so the line does not shift as the rates change: a
  // coloured letter and a value left-aligned in its own box.
  rate_slots_ = Box(root_);
  lv_obj_set_size(rate_slots_, LV_PCT(100), 16);
  lv_obj_align(rate_slots_, LV_ALIGN_BOTTOM_MID, 0, 0);
  const uint32_t colors[2] = {theme::kRateFirst, theme::kRateSecond};
  const char* letters[2] = {"R", "W"};
  for (int i = 0; i < 2; ++i) {
    const int32_t x = 2 + i * 46;
    lv_label_set_text(Placed(rate_slots_, &lv_font_montserrat_12, colors[i], x, 0), letters[i]);
    rate_value_[i] = Placed(rate_slots_, &lv_font_montserrat_12, theme::kMuted, x + 14, 0, 31);
  }
}

void MetricCard::BuildPanel(lv_color_t accent) {
  title_label_ = Line(root_);
  theme::StyleCaption(title_label_);
  lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(root_, 1, 0);
  value_label_ = Line(root_);
  note_label_ = Line(root_);
  theme::StyleCaption(note_label_);
  // The bar sits on the card's bottom edge, outside the text flow.
  bar_ = lv_bar_create(root_);
  lv_obj_add_flag(bar_, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_size(bar_, LV_PCT(100), 4);
  lv_bar_set_range(bar_, 0, 1000);
  lv_obj_set_style_bg_color(bar_, theme::Color(theme::kTrack), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_, accent, LV_PART_INDICATOR);
  lv_obj_set_style_anim_duration(bar_, kDecayAnimationMs, 0);
}

void MetricCard::BuildBattery(lv_color_t accent) {
  // A drawn cell with a nub, filled to the charge.
  cell_ = Box(root_);
  lv_obj_set_pos(cell_, 0, 6);
  lv_obj_set_size(cell_, 44, 26);
  lv_obj_set_style_radius(cell_, 5, 0);
  lv_obj_set_style_border_color(cell_, theme::Color(theme::kMuted), 0);
  lv_obj_set_style_border_width(cell_, 2, 0);
  lv_obj_t* nub = Box(root_);
  lv_obj_set_pos(nub, 44, 14);
  lv_obj_set_size(nub, 4, 10);
  lv_obj_set_style_radius(nub, 1, 0);
  lv_obj_set_style_bg_color(nub, theme::Color(theme::kMuted), 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  bar_ = Meter(cell_, 2, 2, 36, 18, accent);
  lv_obj_set_style_bg_opa(bar_, LV_OPA_TRANSP, LV_PART_MAIN);
  bolt_ = Placed(cell_, &lv_font_montserrat_14, theme::kText, 0, 0);
  lv_label_set_text(bolt_, LV_SYMBOL_CHARGE);
  lv_obj_center(bolt_);

  value_label_ = Placed(root_, &lv_font_montserrat_18, theme::kText, kTextLeft, 0, 146 - kTextLeft);
  note_label_ =
      Placed(root_, &lv_font_montserrat_12, theme::kMuted, kTextLeft, 22, 146 - kTextLeft);

  meter_caption_ = Placed(root_, &lv_font_montserrat_12, theme::kMuted, 0, 48, 50);
  meter_ = Meter(root_, 52, 53, 56, 5, theme::Color(theme::kViolet));
  meter_value_ = Placed(root_, &lv_font_montserrat_12, theme::kText, 110, 48, 36);
  lv_obj_set_style_text_align(meter_value_, LV_TEXT_ALIGN_RIGHT, 0);
}

void MetricCard::BuildRates() {
  wifi_glyph_ = Placed(root_, &lv_font_montserrat_12, theme::kMuted, 0, 0);
  lv_label_set_text(wifi_glyph_, LV_SYMBOL_WIFI);
  note_label_ = Placed(root_, &lv_font_montserrat_12, theme::kMuted, 0, 0, 146);
  const uint32_t colors[2] = {theme::kRateFirst, theme::kRateSecond};
  const char* arrows[2] = {LV_SYMBOL_DOWN, LV_SYMBOL_UP};
  for (int i = 0; i < 2; ++i) {
    const int32_t y = 20 + i * 23;
    lv_label_set_text(Placed(root_, &lv_font_montserrat_14, colors[i], 0, y), arrows[i]);
    rate_value_[i] = Placed(root_, &lv_font_montserrat_14, theme::kText, kRowValueX, y,
                            kRowMeterX - kRowValueX - 2);
    rate_meter_[i] = Meter(root_, kRowMeterX, y + 5, 146 - kRowMeterX, 6, theme::Color(colors[i]));
  }
  value_label_ = rate_value_[0];
}

MetricCard::~MetricCard() {
  if (root_ != nullptr) {
    lv_anim_delete(root_, SetOpacity);
    if (arc_ != nullptr) lv_anim_delete(arc_, SetArcValue);
    lv_obj_delete(root_);
  }
}

void MetricCard::Update(const firmware::domain::MetricCardView& view) {
  if (title_label_ != nullptr) lv_label_set_text(title_label_, view.title.c_str());
  const bool dim_changed = view.dimmed != dimmed_;
  dimmed_ = view.dimmed;
  switch (layout_) {
    case CardLayout::kDial:
      UpdateDial(view, dim_changed);
      break;
    case CardLayout::kPanel: {
      lv_label_set_text(value_label_, view.value.c_str());
      lv_label_set_text(note_label_, view.note.c_str());
      // No value means no bar: an empty gauge would read as a measured zero.
      const bool gauge = view.has_value && view.show_bar;
      SetHidden(bar_, !gauge);
      SetMeter(bar_, gauge && !dimmed_ ? view.bar_per_mille : 0, dim_changed && dimmed_);
      break;
    }
    case CardLayout::kBattery:
      UpdateBattery(view, dim_changed);
      break;
    case CardLayout::kRates:
      UpdateRates(view, dim_changed);
      break;
  }
  if (dim_changed) {
    Animate(root_, SetOpacity, lv_obj_get_style_opa(root_, 0), dimmed_ ? LV_OPA_50 : LV_OPA_COVER,
            kDimAnimationMs);
  }
}

void MetricCard::UpdateDial(const firmware::domain::MetricCardView& view, bool dim_changed) {
  lv_label_set_text(value_label_, view.value.c_str());
  lv_label_set_text(note_label_, view.note.c_str());
  const bool rates = view.rates.has_value();
  SetHidden(rate_slots_, !rates);
  SetHidden(note_label_, rates);
  if (rates) {
    lv_label_set_text(rate_value_[0], view.rates->first.c_str());
    lv_label_set_text(rate_value_[1], view.rates->second.c_str());
  }
  SetHidden(arc_, !view.has_value);
  // Dimmed, "Last 62%" is two lines, so it drops to 14 pt to fit the ring.
  lv_obj_set_style_text_font(value_label_,
                             dimmed_ ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
  // A retained reading decays to the neutral minimum once, on the way to dim;
  // its text still carries the last value, labelled "Last".
  if (dim_changed && dimmed_) {
    Animate(arc_, SetArcValue, lv_arc_get_value(arc_), 0, kDecayAnimationMs);
  } else if (!dimmed_) {
    lv_anim_delete(arc_, SetArcValue);
    lv_arc_set_value(arc_, view.has_value ? view.bar_per_mille : 0);
  }
}

void MetricCard::UpdateBattery(const firmware::domain::MetricCardView& view, bool /*dim_changed*/) {
  lv_label_set_text(value_label_, view.value.c_str());
  // The detail under the value, or the cause when there is no value.
  lv_label_set_text(note_label_, view.has_value ? view.detail.c_str() : view.note.c_str());
  // No value: an empty, faded outline and no fill, never a flat battery.
  SetHidden(bar_, !view.has_value);
  lv_obj_set_style_opa(cell_, view.has_value ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_bar_set_value(bar_, view.bar_per_mille, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_, dimmed_ ? theme::Color(theme::kMuted) : accent_,
                            LV_PART_INDICATOR);
  SetHidden(bolt_, !(view.has_value && view.charging));

  const bool line = view.has_value && !view.meter_caption.empty();
  SetHidden(meter_caption_, !line);
  SetHidden(meter_, !(line && view.show_meter));
  SetHidden(meter_value_, !(line && !view.meter_value.empty()));
  lv_label_set_text(meter_caption_, view.meter_caption.c_str());
  lv_label_set_text(meter_value_, view.meter_value.c_str());
  // A cause with no value or bar ("Health Unavailable") takes the whole line.
  lv_obj_set_width(meter_caption_, view.show_meter || !view.meter_value.empty() ? 50 : 146);
  lv_bar_set_value(meter_, view.meter_per_mille, LV_ANIM_OFF);
}

void MetricCard::UpdateRates(const firmware::domain::MetricCardView& view, bool dim_changed) {
  // The WiFi glyph stands for the word; other links are named in full.
  SetHidden(wifi_glyph_, !view.wifi);
  const int32_t x = view.wifi ? 18 : 0;
  lv_obj_set_x(note_label_, x);
  lv_obj_set_width(note_label_, 146 - x);
  lv_label_set_text(note_label_, view.note.c_str());
  const bool rates = view.rates.has_value();
  lv_label_set_text(rate_value_[0], rates ? view.rates->first.c_str() : "--");
  lv_label_set_text(rate_value_[1], rates ? view.rates->second.c_str() : "--");
  const int32_t levels[2] = {rates ? view.rates->first_per_mille : 0,
                             rates ? view.rates->second_per_mille : 0};
  for (int i = 0; i < 2; ++i) {
    SetHidden(rate_meter_[i], !rates);
    SetMeter(rate_meter_[i], levels[i], dim_changed && dimmed_);
  }
}

}  // namespace firmware::ui::system
