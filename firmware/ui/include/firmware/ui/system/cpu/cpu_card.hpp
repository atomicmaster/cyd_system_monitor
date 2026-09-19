// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/cpu_view.hpp"

namespace firmware::ui::system {

// TB01's initial shell: a persistent status bar (connection + clock) above one
// CPU card. It renders a CpuView and owns no state machine; C02 will add the
// remaining cards beside it through the same Update(view) shape.
class CpuScreen {
 public:
  explicit CpuScreen(lv_obj_t* parent);
  ~CpuScreen();
  CpuScreen(const CpuScreen&) = delete;
  CpuScreen& operator=(const CpuScreen&) = delete;

  void Update(const firmware::domain::CpuView& view);

  bool dimmed() const { return dimmed_; }
  lv_obj_t* connection_label() const { return connection_label_; }
  lv_obj_t* clock_label() const { return clock_label_; }
  lv_obj_t* value_label() const { return value_label_; }
  lv_obj_t* note_label() const { return note_label_; }
  lv_obj_t* bar() const { return bar_; }

 private:
  lv_obj_t* root_ = nullptr;
  lv_obj_t* connection_label_ = nullptr;
  lv_obj_t* clock_label_ = nullptr;
  lv_obj_t* card_ = nullptr;
  lv_obj_t* value_label_ = nullptr;
  lv_obj_t* note_label_ = nullptr;
  lv_obj_t* bar_ = nullptr;
  bool dimmed_ = false;
};

}  // namespace firmware::ui::system
