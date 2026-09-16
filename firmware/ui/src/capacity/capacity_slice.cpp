// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/capacity/capacity_slice.hpp"

#include <algorithm>
#include <utility>

#include "firmware/domain/capacity.hpp"

namespace firmware::ui::capacity {

namespace {

namespace bounds = firmware::domain::capacity;

std::string Truncate(const std::string& text, size_t max_len) {
  return text.size() <= max_len ? text : text.substr(0, max_len);
}

lv_obj_t* MakeScreenRoot(lv_obj_t* parent) {
  lv_obj_t* root = lv_obj_create(parent);
  lv_obj_set_size(root, 320, 240);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root, 4, 0);
  lv_obj_set_style_pad_row(root, 2, 0);
  return root;
}

lv_obj_t* MakeLabel(lv_obj_t* parent) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  return label;
}

lv_obj_t* MakeScrollableList(lv_obj_t* parent) {
  lv_obj_t* list = lv_obj_create(parent);
  lv_obj_set_width(list, LV_PCT(100));
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  return list;
}

}  // namespace

CapacitySliceController::CapacitySliceController(lv_obj_t* parent, CapacitySliceState state)
    : parent_(parent), state_(std::move(state)) {
  ShowLiveStatus();
}

CapacitySliceController::~CapacitySliceController() { DestroyCurrent(); }

void CapacitySliceController::DestroyCurrent() {
  if (root_ != nullptr) {
    lv_obj_delete(root_);
    root_ = nullptr;
  }
  live_status_label_ = nullptr;
  alert_list_ = nullptr;
  alert_detail_title_ = nullptr;
  alert_detail_body_ = nullptr;
  settings_list_ = nullptr;
}

void CapacitySliceController::ShowLiveStatus() {
  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  live_status_label_ = MakeLabel(root_);
  lv_label_set_text(live_status_label_,
                    Truncate(state_.live_status_text, bounds::kMaxLiveStatusTextLength).c_str());
  current_screen_ = CapacityScreen::kLiveStatus;
  live_status_refreshed_once_ = false;
}

bool CapacitySliceController::UpdateLiveStatus(std::string live_status_text, uint32_t now_ms) {
  state_.live_status_text = std::move(live_status_text);
  if (current_screen_ != CapacityScreen::kLiveStatus || live_status_label_ == nullptr) {
    return false;
  }

  const uint32_t elapsed_ms = now_ms - last_live_status_refresh_ms_;
  if (live_status_refreshed_once_ && elapsed_ms < bounds::kMinLiveStatusRefreshIntervalMs) {
    return false;
  }

  lv_label_set_text(live_status_label_,
                    Truncate(state_.live_status_text, bounds::kMaxLiveStatusTextLength).c_str());
  last_live_status_refresh_ms_ = now_ms;
  live_status_refreshed_once_ = true;
  return true;
}

void CapacitySliceController::ShowAlertList() {
  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  alert_list_ = MakeScrollableList(root_);

  const size_t row_count = std::min<size_t>(state_.alerts.size(), bounds::kMaxAlertListRows);
  for (size_t i = 0; i < row_count; ++i) {
    const AlertRow& alert = state_.alerts[i];
    lv_obj_t* row = MakeLabel(alert_list_);
    const std::string title = Truncate(alert.title, bounds::kMaxAlertLabelLength);
    lv_label_set_text_fmt(row, "[%s] %s", alert.severity.c_str(), title.c_str());
  }
  current_screen_ = CapacityScreen::kAlertList;
}

void CapacitySliceController::ShowAlertDetail(size_t alert_index) {
  const size_t row_count = std::min<size_t>(state_.alerts.size(), bounds::kMaxAlertListRows);
  if (alert_index >= row_count) {
    return;
  }

  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  const AlertRow& alert = state_.alerts[alert_index];

  alert_detail_title_ = MakeLabel(root_);
  const std::string bounded_title = Truncate(alert.title, bounds::kMaxAlertLabelLength);
  lv_label_set_text_fmt(alert_detail_title_, "[%s] %s", alert.severity.c_str(),
                        bounded_title.c_str());

  alert_detail_body_ = MakeLabel(root_);
  lv_label_set_text(alert_detail_body_,
                    Truncate(alert.detail, bounds::kMaxAlertDetailLength).c_str());

  current_screen_ = CapacityScreen::kAlertDetail;
}

void CapacitySliceController::AdvanceScreen() {
  switch (current_screen_) {
    case CapacityScreen::kLiveStatus:
      ShowAlertList();
      return;
    case CapacityScreen::kAlertList:
      ShowAlertDetail(0);
      // An empty alert list has nothing to show detail for; ShowAlertDetail
      // is then a no-op and current_screen_ stays kAlertList, so fall
      // through to settings instead of getting stuck.
      if (current_screen_ == CapacityScreen::kAlertList) {
        ShowSettings();
      }
      return;
    case CapacityScreen::kAlertDetail:
      ShowSettings();
      return;
    case CapacityScreen::kSettings:
      ShowLiveStatus();
      return;
  }
}

void CapacitySliceController::ShowSettings() {
  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  settings_list_ = MakeScrollableList(root_);

  const size_t row_count = std::min<size_t>(state_.settings.size(), bounds::kMaxSettingsRows);
  for (size_t i = 0; i < row_count; ++i) {
    const SettingRow& setting = state_.settings[i];
    lv_obj_t* row = MakeLabel(settings_list_);
    const std::string label = Truncate(setting.label, bounds::kMaxSettingLabelLength);
    const std::string value = Truncate(setting.value, bounds::kMaxSettingValueLength);
    lv_label_set_text_fmt(row, "%s: %s", label.c_str(), value.c_str());
  }
  current_screen_ = CapacityScreen::kSettings;
}

CapacitySliceState MakeMaxRepresentativeState() {
  CapacitySliceState state;
  state.live_status_text = "cpu:-- ram:-- net:-- (placeholder, no Host source yet)";

  for (uint32_t i = 0; i < bounds::kMaxAlertListRows; ++i) {
    AlertRow row;
    row.severity = (i % 2 == 0) ? "Medium" : "Low";
    row.title = "placeholder alert " + std::to_string(i);
    row.detail = "placeholder detail text exercising the declared maximum detail length for row " +
                 std::to_string(i);
    state.alerts.push_back(std::move(row));
  }

  for (uint32_t i = 0; i < bounds::kMaxSettingsRows; ++i) {
    SettingRow row;
    row.label = "setting_" + std::to_string(i);
    row.value = "placeholder";
    state.settings.push_back(std::move(row));
  }

  return state;
}

}  // namespace firmware::ui::capacity
