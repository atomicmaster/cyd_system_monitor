// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/cpu/cpu_card.hpp"

namespace firmware::ui::system {
namespace {

constexpr lv_opa_t kLiveOpacity = LV_OPA_COVER;
constexpr lv_opa_t kDimmedOpacity = LV_OPA_50;
constexpr uint32_t kDimAnimationMs = 300;

void SetOpacity(void* object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

// The card fades toward the dimmed level; the value is never animated toward
// zero, since a stale reading is retained, not measured to be zero.
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

CpuScreen::CpuScreen(lv_obj_t* parent) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 320, 240);
  lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root_, 4, 0);
  lv_obj_set_style_pad_row(root_, 4, 0);

  lv_obj_t* bar_row = lv_obj_create(root_);
  lv_obj_set_size(bar_row, LV_PCT(100), 28);
  lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  connection_label_ = lv_label_create(bar_row);
  clock_label_ = lv_label_create(bar_row);

  card_ = lv_obj_create(root_);
  lv_obj_set_size(card_, LV_PCT(100), 96);
  lv_obj_set_flex_flow(card_, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* title = lv_label_create(card_);
  lv_label_set_text(title, "CPU");
  value_label_ = lv_label_create(card_);
  note_label_ = lv_label_create(card_);
  bar_ = lv_bar_create(card_);
  lv_obj_set_size(bar_, LV_PCT(100), 10);
  lv_bar_set_range(bar_, 0, 1000);
  Update(firmware::domain::CpuView{
      .value = "--", .connection = "Waiting for Host", .clock = "Time unavailable"});
}

CpuScreen::~CpuScreen() {
  if (root_ != nullptr) {
    lv_anim_delete(card_, SetOpacity);
    lv_obj_delete(root_);
  }
}

void CpuScreen::Update(const firmware::domain::CpuView& view) {
  lv_label_set_text(connection_label_, view.connection.c_str());
  lv_label_set_text(clock_label_, view.clock.c_str());
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
    FadeTo(card_, dimmed_ ? kDimmedOpacity : kLiveOpacity);
  }
}

}  // namespace firmware::ui::system
