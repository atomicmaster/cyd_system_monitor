// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "firmware/domain/agents_bar.hpp"
#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/link_view.hpp"
#include "firmware/ui/system/settings/settings_panel.hpp"
#include "firmware/ui/system/system_screen.hpp"

namespace firmware::ui::system {

// The Overview (SystemScreen) plus TB02's minimal pairing prompt and the Host link panel
// (versions, pairing, and any fault), opened by tapping the connection text. A fault also
// replaces the Agents bar, so its cause stays visible without a tap. Renders CpuView/LinkView
// only; the Confirm/Decline buttons report the touch decision to the caller, which owns the link.
class LinkScreen {
 public:
  using DecisionCallback = std::function<void(bool accepted)>;
  // `on_settings` is called with every change made in the clock settings panel
  // (opened by tapping the status bar); persisting it is the caller's job.
  LinkScreen(lv_obj_t* parent, DecisionCallback on_decision,
             SettingsPanel::ChangeCallback on_settings,
             const firmware::domain::DisplaySettings& initial,
             SettingsPanel::Actions settings_actions = {});
  ~LinkScreen();
  LinkScreen(const LinkScreen&) = delete;
  LinkScreen& operator=(const LinkScreen&) = delete;

  void Update(const firmware::domain::CpuView& status,
              std::span<const firmware::domain::MetricCardView> cards,
              const firmware::domain::LinkView& link,
              const firmware::domain::AgentsBarView& agents = {}, const std::string& coverage = {});
  const SystemScreen& system() const { return system_; }
  SystemScreen& system() { return system_; }
  SettingsPanel& settings_panel() { return *settings_panel_; }
  const firmware::domain::DisplaySettings& settings() const { return settings_; }

  lv_obj_t* prompt_panel() const { return prompt_panel_; }
  lv_obj_t* about_panel() const { return about_panel_; }
  bool about_visible() const { return !lv_obj_has_flag(about_panel_, LV_OBJ_FLAG_HIDDEN); }
  lv_obj_t* about_done_button() const { return about_done_; }
  // The Host link rows (a label and a value each), one per LinkView detail.
  lv_obj_t* details_list() const { return details_; }
  lv_obj_t* code_label() const { return code_label_; }
  lv_obj_t* fault_label() const { return fault_label_; }

 private:
  struct Target {
    LinkScreen* screen = nullptr;
    bool accepted = false;
  };
  void MakeButton(const char* text, lv_align_t align, Target* target);
  void ShowDetails();
  SystemScreen system_;
  firmware::domain::DisplaySettings settings_;
  std::unique_ptr<SettingsPanel> settings_panel_;
  DecisionCallback on_decision_;
  Target confirm_target_;
  Target decline_target_;
  lv_obj_t* about_panel_ = nullptr;
  lv_obj_t* about_done_ = nullptr;
  lv_obj_t* details_ = nullptr;
  std::vector<firmware::domain::LinkDetail> latest_details_;
  std::vector<firmware::domain::LinkDetail> shown_details_;
  lv_obj_t* fault_label_ = nullptr;
  lv_obj_t* prompt_panel_ = nullptr;
  lv_obj_t* code_label_ = nullptr;
};

}  // namespace firmware::ui::system
