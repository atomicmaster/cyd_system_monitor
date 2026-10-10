// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/agents/agents_bar.hpp"

#include <cstdint>

#include "firmware/ui/system/agents/mascots.hpp"
#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {
namespace {

using firmware::domain::AgentActivity;

constexpr int32_t kContentWidth = 308;  // the 312 px Overview row less 2 px padding each side
constexpr uint32_t kBeatMs = 300;       // half a dance step / cursor blink
constexpr uint32_t kRockMs = 420;       // the Grok mark's swing to one side
constexpr int32_t kRockTenthsOfDegree = 150;
constexpr uint32_t kDimAnimationMs = 300;

void SetOpacity(void* object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

// Two-frame animation: the first half of each beat shows the mascot, the
// second its busy frame. The slot is the image's user data.
void SetFrame(void* object, int32_t value) {
  auto* image = static_cast<lv_obj_t*>(object);
  const auto slot = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(image)));
  const void* frame = value < 50 ? &mascots::Image(slot) : &mascots::BusyFrame(slot);
  if (lv_image_get_src(image) != frame) lv_image_set_src(image, frame);
}

void SetRock(void* object, int32_t value) {
  lv_image_set_rotation(static_cast<lv_obj_t*>(object), value);
}

void Loop(lv_obj_t* object, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_exec_cb(&animation, exec);
  lv_anim_set_values(&animation, from, to);
  lv_anim_set_duration(&animation, ms);
  lv_anim_set_playback_duration(&animation, ms);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
  lv_anim_start(&animation);
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
  // No panel: the mascots sit straight on the screen (the Quiet style).
  root_ = lv_obj_create(parent);
  lv_obj_remove_style_all(root_);
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(root_, width, height);
  lv_obj_set_style_pad_hor(root_, 2, 0);
  lv_obj_set_style_text_color(root_, theme::Color(theme::kText), 0);

  for (size_t slot = 0; slot < mascots_.size(); ++slot) {
    lv_obj_t* image = lv_image_create(root_);
    lv_image_set_src(image, &mascots::Image(slot));
    lv_obj_set_user_data(image, reinterpret_cast<void*>(static_cast<uintptr_t>(slot)));
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    // Each mascot centred in its own third of the bar, so none moves when
    // another starts or stops.
    const int32_t centre = static_cast<int32_t>(2 * slot + 1) * kContentWidth / 6;
    lv_obj_align(image, LV_ALIGN_LEFT_MID,
                 centre - static_cast<int32_t>(mascots::Image(slot).header.w) / 2, 0);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    mascots_[slot].image = image;
  }

  message_label_ = lv_label_create(root_);
  theme::StyleCaption(message_label_);
  notice_label_ = lv_label_create(root_);
  lv_obj_set_style_text_font(notice_label_, &lv_font_montserrat_12, 0);
  lv_obj_set_width(notice_label_, LV_PCT(100));
  lv_label_set_long_mode(notice_label_, LV_LABEL_LONG_DOT);
  lv_obj_align(notice_label_, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_add_flag(notice_label_, LV_OBJ_FLAG_HIDDEN);
  Update(firmware::domain::AgentsBarView{.message = "--"});
}

AgentsBar::~AgentsBar() {
  if (root_ == nullptr) return;
  lv_anim_delete(root_, SetOpacity);
  for (Mascot& mascot : mascots_) {
    lv_anim_delete(mascot.image, SetFrame);
    lv_anim_delete(mascot.image, SetRock);
  }
  lv_obj_delete(root_);
}

void AgentsBar::Set(size_t slot, State state) {
  Mascot& mascot = mascots_[slot];
  if (mascot.state == state) return;
  mascot.state = state;
  lv_obj_t* image = mascot.image;
  lv_anim_delete(image, SetFrame);
  lv_anim_delete(image, SetRock);
  lv_image_set_src(image, &mascots::Image(slot));
  lv_image_set_rotation(image, 0);
  SetHidden(image, state == State::kHidden);
  lv_obj_set_style_image_recolor(
      image, theme::Color(state == State::kSilhouette ? theme::kTrack : mascots::Color(slot)), 0);
  if (state != State::kBusy) return;
  if (slot == 2) {
    Loop(image, SetRock, -kRockTenthsOfDegree, kRockTenthsOfDegree, kRockMs);
  } else {
    Loop(image, SetFrame, 0, 100, kBeatMs);
  }
}

void AgentsBar::Update(const firmware::domain::AgentsBarView& view, const std::string& notice) {
  const bool noticing = !notice.empty();
  // "--" and its cause: no reading, so no mascot at all.
  const bool reading = !noticing && view.message.rfind("--", 0) != 0;
  SetHidden(notice_label_, !noticing);
  lv_label_set_text(notice_label_, notice.c_str());
  lv_label_set_text(message_label_, view.message.c_str());
  SetHidden(message_label_, noticing || view.message.empty());
  // The cause reads from the left; "Last" sits at the right end, clear of the mascots.
  lv_obj_align(message_label_, reading ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);

  std::array<const firmware::domain::AgentBadge*, cyd::protocol::kAgentSlots> running{};
  for (const auto& badge : view.badges) {
    if (badge.slot < running.size()) running[badge.slot] = &badge;
  }
  for (size_t slot = 0; slot < mascots_.size(); ++slot) {
    State state = State::kHidden;
    if (reading && running[slot] == nullptr) {
      state = State::kSilhouette;
    } else if (reading) {
      // A retained reading is not activity: every mascot is still.
      state = running[slot]->activity == AgentActivity::kBusy && !view.dimmed ? State::kBusy
                                                                              : State::kStill;
    }
    Set(slot, state);
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

bool AgentsBar::mascot_visible(size_t slot) const {
  return !lv_obj_has_flag(mascots_[slot].image, LV_OBJ_FLAG_HIDDEN);
}

bool AgentsBar::animating(size_t slot) const {
  lv_obj_t* image = mascots_[slot].image;
  return lv_anim_get(image, SetFrame) != nullptr || lv_anim_get(image, SetRock) != nullptr;
}

}  // namespace firmware::ui::system
