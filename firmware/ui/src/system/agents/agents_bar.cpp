// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/agents/agents_bar.hpp"

#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {
namespace {

using firmware::domain::AgentActivity;

constexpr int32_t kIconSize = 14;
constexpr int32_t kDotSize = 7;
constexpr uint32_t kPulseMs = 900;
constexpr uint32_t kSpinMs = 1000;
constexpr uint32_t kDimAnimationMs = 300;

void SetOpacity(void* object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

void StartPulse(lv_obj_t* dot) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, dot);
  lv_anim_set_exec_cb(&animation, SetOpacity);
  lv_anim_set_values(&animation, LV_OPA_30, LV_OPA_COVER);
  lv_anim_set_duration(&animation, kPulseMs);
  lv_anim_set_playback_duration(&animation, kPulseMs);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&animation);
}

void StopPulse(lv_obj_t* dot) {
  lv_anim_delete(dot, SetOpacity);
  lv_obj_set_style_opa(dot, LV_OPA_COVER, 0);
}

void SetHidden(lv_obj_t* object, bool hidden) {
  if (hidden) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace

AgentsBar::AgentsBar(lv_obj_t* parent, int32_t width, int32_t height) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, width, height);
  theme::StylePanel(root_);
  lv_obj_set_style_pad_hor(root_, 6, 0);
  lv_obj_set_style_pad_ver(root_, 0, 0);
  lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(root_, 5, 0);

  title_label_ = lv_label_create(root_);
  theme::StyleCaption(title_label_);
  lv_label_set_text(title_label_, "Agents");
  message_label_ = lv_label_create(root_);
  lv_obj_set_style_text_font(message_label_, &lv_font_montserrat_12, 0);

  for (size_t slot = 0; slot < badges_.size(); ++slot) {
    Badge& badge = badges_[slot];
    const lv_color_t accent = theme::AgentAccent(slot);
    badge.box = lv_obj_create(root_);
    lv_obj_remove_style_all(badge.box);
    lv_obj_set_size(badge.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(badge.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(badge.box, 3, 0);

    lv_obj_t* icon = lv_obj_create(badge.box);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, kIconSize, kIconSize);
    badge.dot = lv_obj_create(icon);
    lv_obj_remove_style_all(badge.dot);
    lv_obj_set_size(badge.dot, kDotSize, kDotSize);
    lv_obj_center(badge.dot);
    lv_obj_set_style_radius(badge.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge.dot, accent, 0);
    lv_obj_set_style_border_color(badge.dot, accent, 0);
    badge.spinner = lv_spinner_create(icon);
    lv_spinner_set_anim_params(badge.spinner, kSpinMs, 90);
    lv_obj_set_size(badge.spinner, kIconSize, kIconSize);
    lv_obj_center(badge.spinner);
    lv_obj_set_style_arc_width(badge.spinner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(badge.spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(badge.spinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(badge.spinner, accent, LV_PART_INDICATOR);
    lv_obj_add_flag(badge.spinner, LV_OBJ_FLAG_HIDDEN);

    badge.label = lv_label_create(badge.box);
    lv_obj_set_style_text_font(badge.label, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(badge.box, LV_OBJ_FLAG_HIDDEN);
  }

  notice_label_ = lv_label_create(root_);
  lv_obj_set_style_text_font(notice_label_, &lv_font_montserrat_12, 0);
  lv_obj_set_flex_grow(notice_label_, 1);
  lv_label_set_long_mode(notice_label_, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(notice_label_, LV_OBJ_FLAG_HIDDEN);
  Update(firmware::domain::AgentsBarView{.message = "--"});
}

AgentsBar::~AgentsBar() {
  if (root_ == nullptr) return;
  lv_anim_delete(root_, SetOpacity);
  for (Badge& badge : badges_) lv_anim_delete(badge.dot, SetOpacity);
  lv_obj_delete(root_);
}

void AgentsBar::Show(Badge& badge, AgentActivity activity, bool still) {
  if (badge.shown && badge.activity == activity && badge.still == still) return;
  badge.shown = true;
  badge.activity = activity;
  badge.still = still;
  lv_obj_remove_flag(badge.box, LV_OBJ_FLAG_HIDDEN);
  const bool idle = activity == AgentActivity::kIdle;
  // Idle is a hollow ring; unknown and busy are a filled dot.
  lv_obj_set_style_bg_opa(badge.dot, idle ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(badge.dot, idle ? 2 : 0, 0);
  SetHidden(badge.spinner, still || activity != AgentActivity::kBusy);
  if (!still && activity == AgentActivity::kUnknown) {
    StartPulse(badge.dot);
  } else {
    StopPulse(badge.dot);
  }
}

void AgentsBar::Update(const firmware::domain::AgentsBarView& view, const std::string& notice) {
  const bool noticing = !notice.empty();
  SetHidden(notice_label_, !noticing);
  // The badges name themselves; the title is for the message alone ("Agents
  // None running"), and leaves room for "Last" and three badges.
  SetHidden(title_label_, noticing || !view.badges.empty());
  lv_label_set_text(notice_label_, notice.c_str());
  lv_label_set_text(message_label_, view.message.c_str());
  SetHidden(message_label_, noticing || view.message.empty());

  std::array<const firmware::domain::AgentBadge*, cyd::protocol::kAgentSlots> by_slot{};
  for (const auto& badge : view.badges) {
    if (badge.slot < by_slot.size()) by_slot[badge.slot] = &badge;
  }
  for (size_t slot = 0; slot < badges_.size(); ++slot) {
    Badge& badge = badges_[slot];
    if (noticing || by_slot[slot] == nullptr) {
      if (badge.shown) {
        badge.shown = false;
        StopPulse(badge.dot);
        lv_obj_add_flag(badge.spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(badge.box, LV_OBJ_FLAG_HIDDEN);
      }
      continue;
    }
    lv_label_set_text(badge.label, by_slot[slot]->label.c_str());
    Show(badge, by_slot[slot]->activity, view.dimmed);
  }

  const bool dimmed = view.dimmed && !noticing;
  if (dimmed != dimmed_) {
    dimmed_ = dimmed;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, root_);
    lv_anim_set_exec_cb(&animation, SetOpacity);
    lv_anim_set_values(&animation, lv_obj_get_style_opa(root_, 0),
                       dimmed_ ? LV_OPA_50 : LV_OPA_COVER);
    lv_anim_set_duration(&animation, kDimAnimationMs);
    lv_anim_start(&animation);
  }
}

bool AgentsBar::badge_visible(size_t slot) const {
  return !lv_obj_has_flag(badges_[slot].box, LV_OBJ_FLAG_HIDDEN);
}

bool AgentsBar::pulsing(size_t slot) const {
  return lv_anim_get(badges_[slot].dot, SetOpacity) != nullptr;
}

bool AgentsBar::spinning(size_t slot) const {
  return badge_visible(slot) && !lv_obj_has_flag(badges_[slot].spinner, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace firmware::ui::system
