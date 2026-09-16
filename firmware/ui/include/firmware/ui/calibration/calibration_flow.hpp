// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <array>
#include <optional>

#include "firmware/domain/calibration/calibration.hpp"

namespace firmware::ui::calibration {

// Separate validation targets from firmware::domain::calibration's five
// guided calibration targets, per F06's acceptance line "physical taps
// validate independently of calibration targets." Edge midpoints, inset by
// the same margin as the calibration corners, landscape 320x240.
inline constexpr std::array<firmware::domain::calibration::ScreenPoint, 4> kValidationTargets = {{
    {160, firmware::domain::calibration::kCalibrationMarginPx},        // top-mid
    {320 - firmware::domain::calibration::kCalibrationMarginPx, 120},  // right-mid
    {160, 240 - firmware::domain::calibration::kCalibrationMarginPx},  // bottom-mid
    {firmware::domain::calibration::kCalibrationMarginPx, 120},        // left-mid
}};

// Drives the guided 5-point calibration flow followed by a separate
// validation pass, as an explicit state machine so the simulator (and
// later, tests) can inject synthetic RawTouchSamples deterministically
// without any real touch hardware. The platform touch driver calls
// SubmitRawSample() once per touch-down; this class owns all advancement
// logic and delegates the actual math to firmware::domain::calibration.
class CalibrationFlow {
 public:
  enum class Stage {
    kGuiding,     // presenting calibration target current_target_index() of 5
    kValidating,  // transform computed; presenting validation target current_target_index()
    kAccepted,    // validation passed every target; transform() is ready to persist
    kRejected,    // degenerate fit or a validation tap exceeded max_error_px; call Reset()
  };

  // Builds the calibration screen (a target-position label plus a status
  // label) under `parent`. `max_error_px` is forwarded to ValidateTap for
  // every validation target.
  explicit CalibrationFlow(
      lv_obj_t* parent,
      double max_error_px = firmware::domain::calibration::kDefaultMaxValidationErrorPx);

  Stage stage() const { return stage_; }
  // Index into kCalibrationTargets (kGuiding) or kValidationTargets
  // (kValidating). Meaningless in kAccepted/kRejected.
  int current_target_index() const { return current_target_index_; }
  double max_error_px() const { return max_error_px_; }
  double last_validation_error_px() const { return last_validation_error_px_; }
  const std::optional<firmware::domain::calibration::AffineTransform>& transform() const {
    return transform_;
  }

  lv_obj_t* root() const { return root_; }
  lv_obj_t* status_label() const { return status_label_; }

  // Called once per touch-down with the controller's raw sample for
  // whichever target is currently presented. Advances the state machine;
  // see Stage's comments for the transitions.
  void SubmitRawSample(firmware::domain::calibration::RawTouchSample raw);

  // Returns to kGuiding at target 0, discarding any partial progress.
  // Used both for an operator-initiated recalibration and to retry after
  // kRejected.
  void Reset();

 private:
  void UpdateStatusLabel();

  lv_obj_t* root_ = nullptr;
  lv_obj_t* status_label_ = nullptr;

  Stage stage_ = Stage::kGuiding;
  int current_target_index_ = 0;
  double max_error_px_;
  double last_validation_error_px_ = 0;

  std::array<firmware::domain::calibration::RawTouchSample, 5> guided_samples_{};
  std::optional<firmware::domain::calibration::AffineTransform> transform_;
};

}  // namespace firmware::ui::calibration
