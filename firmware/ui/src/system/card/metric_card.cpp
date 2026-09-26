// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/card/metric_card.hpp"

#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {
namespace {

constexpr uint32_t kDimAnimationMs = 300;
constexpr uint32_t kDecayAnimationMs = 800;
constexpr int32_t kDialSize = 64;
constexpr int32_t kDialTop = 13;
constexpr int32_t kDialWidth = 6;

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

// Fixed-height lines: text that is too long is clipped, never wrapped, so a
// card cannot grow past its grid cell.
lv_obj_t* Line(lv_obj_t* parent) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  return label;
}

}  // namespace

MetricCard::MetricCard(lv_obj_t* parent, int32_t width, int32_t height, bool dial,
                       lv_color_t accent) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, width, height);
  theme::StylePanel(root_);
  lv_obj_set_style_pad_all(root_, 3, 0);
  title_label_ = Line(root_);
  theme::StyleCaption(title_label_);
  if (dial) {
    BuildDial(accent);
  } else {
    BuildPanel(accent);
  }
  Update(firmware::domain::MetricCardView{.title = "", .value = "--", .dial = dial});
}

void MetricCard::BuildDial(lv_color_t accent) {
  // A 270-degree arc open at the bottom, like a gauge; the value sits in its
  // centre and the note in the opening below.
  lv_obj_t* holder = lv_obj_create(root_);
  lv_obj_remove_style_all(holder);
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
}

void MetricCard::BuildPanel(lv_color_t accent) {
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

MetricCard::~MetricCard() {
  if (root_ != nullptr) {
    lv_anim_delete(root_, SetOpacity);
    if (arc_ != nullptr) lv_anim_delete(arc_, SetArcValue);
    lv_obj_delete(root_);
  }
}

void MetricCard::Update(const firmware::domain::MetricCardView& view) {
  lv_label_set_text(title_label_, view.title.c_str());
  lv_label_set_text(value_label_, view.value.c_str());
  lv_label_set_text(note_label_, view.note.c_str());
  const bool dim_changed = view.dimmed != dimmed_;
  dimmed_ = view.dimmed;
  // No value means no arc or bar: an empty gauge would read as a measured zero.
  const bool gauge = view.has_value && (arc_ != nullptr || view.show_bar);
  lv_obj_t* meter = arc_ != nullptr ? arc_ : bar_;
  if (gauge) {
    lv_obj_remove_flag(meter, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(meter, LV_OBJ_FLAG_HIDDEN);
  }
  // A retained reading decays to the neutral minimum once, on the way to dim;
  // its text still carries the last value, labelled "Last".
  const int32_t target = gauge && !dimmed_ ? view.bar_per_mille : 0;
  if (arc_ != nullptr) {
    // Dimmed, "Last 62%" is two lines, so it drops to 14 pt to fit the ring.
    lv_obj_set_style_text_font(value_label_,
                               dimmed_ ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
    if (dim_changed && dimmed_) {
      Animate(arc_, SetArcValue, lv_arc_get_value(arc_), 0, kDecayAnimationMs);
    } else if (!dimmed_) {
      lv_anim_delete(arc_, SetArcValue);
      lv_arc_set_value(arc_, target);
    }
  } else {
    lv_bar_set_value(bar_, target, dim_changed && dimmed_ ? LV_ANIM_ON : LV_ANIM_OFF);
  }
  if (dim_changed) {
    Animate(root_, SetOpacity, lv_obj_get_style_opa(root_, 0), dimmed_ ? LV_OPA_50 : LV_OPA_COVER,
            kDimAnimationMs);
  }
}

}  // namespace firmware::ui::system
