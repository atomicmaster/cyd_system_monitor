// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/card/metric_card.hpp"

namespace firmware::ui::system {
namespace {

constexpr uint32_t kDimAnimationMs = 300;

void SetOpacity(void* object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

void FadeTo(lv_obj_t* object, lv_opa_t target) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_exec_cb(&animation, SetOpacity);
  lv_anim_set_values(&animation, lv_obj_get_style_opa(object, 0), target);
  lv_anim_set_duration(&animation, kDimAnimationMs);
  lv_anim_start(&animation);
}

}  // namespace

MetricCard::MetricCard(lv_obj_t* parent, int32_t width, int32_t height) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, width, height);
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root_, 4, 0);
  lv_obj_set_style_pad_row(root_, 2, 0);
  title_label_ = lv_label_create(root_);
  value_label_ = lv_label_create(root_);
  note_label_ = lv_label_create(root_);
  bar_ = lv_bar_create(root_);
  lv_obj_set_size(bar_, LV_PCT(100), 8);
  lv_bar_set_range(bar_, 0, 1000);
  Update(firmware::domain::MetricCardView{.title = "", .value = "--"});
}

MetricCard::~MetricCard() {
  if (root_ != nullptr) {
    lv_anim_delete(root_, SetOpacity);
    lv_obj_delete(root_);
  }
}

void MetricCard::Update(const firmware::domain::MetricCardView& view) {
  lv_label_set_text(title_label_, view.title.c_str());
  lv_label_set_text(value_label_, view.value.c_str());
  lv_label_set_text(note_label_, view.note.c_str());
  // No value means no bar: an empty bar would read as a measured zero.
  if (view.has_value) {
    lv_obj_remove_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(bar_, view.bar_per_mille, LV_ANIM_OFF);
  } else {
    lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
  }
  if (view.dimmed != dimmed_) {
    dimmed_ = view.dimmed;
    FadeTo(root_, dimmed_ ? LV_OPA_50 : LV_OPA_COVER);
  }
}

}  // namespace firmware::ui::system
