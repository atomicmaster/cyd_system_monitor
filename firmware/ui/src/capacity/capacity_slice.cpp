// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/capacity/capacity_slice.hpp"

#include <algorithm>

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
}

void CapacitySliceController::ShowLiveStatus() {
  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  lv_obj_t* label = MakeLabel(root_);
  lv_label_set_text(label,
                    Truncate(state_.live_status_text, bounds::kMaxLiveStatusTextLength).c_str());
  current_screen_ = CapacityScreen::kLiveStatus;
}

void CapacitySliceController::ShowAlertList() {
  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  lv_obj_t* list = MakeScrollableList(root_);

  const size_t row_count = std::min<size_t>(state_.alerts.size(), bounds::kMaxAlertListRows);
  for (size_t i = 0; i < row_count; ++i) {
    const AlertRow& alert = state_.alerts[i];
    lv_obj_t* row = MakeLabel(list);
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

  lv_obj_t* title = MakeLabel(root_);
  const std::string bounded_title = Truncate(alert.title, bounds::kMaxAlertLabelLength);
  lv_label_set_text_fmt(title, "[%s] %s", alert.severity.c_str(), bounded_title.c_str());

  lv_obj_t* detail = MakeLabel(root_);
  lv_label_set_text(detail, Truncate(alert.detail, bounds::kMaxAlertDetailLength).c_str());

  current_screen_ = CapacityScreen::kAlertDetail;
}

void CapacitySliceController::ShowSettings() {
  DestroyCurrent();
  root_ = MakeScreenRoot(parent_);
  lv_obj_t* list = MakeScrollableList(root_);

  const size_t row_count = std::min<size_t>(state_.settings.size(), bounds::kMaxSettingsRows);
  for (size_t i = 0; i < row_count; ++i) {
    const SettingRow& setting = state_.settings[i];
    lv_obj_t* row = MakeLabel(list);
    const std::string label = Truncate(setting.label, bounds::kMaxSettingLabelLength);
    const std::string value = Truncate(setting.value, bounds::kMaxSettingValueLength);
    lv_label_set_text_fmt(row, "%s: %s", label.c_str(), value.c_str());
  }
  current_screen_ = CapacityScreen::kSettings;
}

}  // namespace firmware::ui::capacity
