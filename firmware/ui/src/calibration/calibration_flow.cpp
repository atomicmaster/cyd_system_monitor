// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/calibration/calibration_flow.hpp"

namespace firmware::ui::calibration {

using firmware::domain::calibration::ComputeAffineTransform;
using firmware::domain::calibration::kCalibrationTargets;
using firmware::domain::calibration::ValidateTap;

CalibrationFlow::CalibrationFlow(lv_obj_t* parent, double max_error_px) : max_error_px_(max_error_px) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 320, 240);

  status_label_ = lv_label_create(root_);
  lv_obj_center(status_label_);
  UpdateStatusLabel();
}

void CalibrationFlow::UpdateStatusLabel() {
  switch (stage_) {
    case Stage::kGuiding:
      lv_label_set_text_fmt(status_label_, "Calibrate target %d of %d", current_target_index_ + 1,
                             static_cast<int>(kCalibrationTargets.size()));
      break;
    case Stage::kValidating:
      lv_label_set_text_fmt(status_label_, "Validate target %d of %d", current_target_index_ + 1,
                             static_cast<int>(kValidationTargets.size()));
      break;
    case Stage::kAccepted:
      lv_label_set_text(status_label_, "Calibration accepted");
      break;
    case Stage::kRejected:
      lv_label_set_text(status_label_, "Calibration rejected - retry");
      break;
  }
}

void CalibrationFlow::SubmitRawSample(firmware::domain::calibration::RawTouchSample raw) {
  if (stage_ == Stage::kAccepted || stage_ == Stage::kRejected) {
    // Ignore stray samples once the flow has already concluded; the caller
    // must Reset() to start over.
    return;
  }

  if (stage_ == Stage::kGuiding) {
    guided_samples_[static_cast<size_t>(current_target_index_)] = raw;
    ++current_target_index_;
    if (current_target_index_ < static_cast<int>(kCalibrationTargets.size())) {
      UpdateStatusLabel();
      return;
    }

    // Fifth point collected: compute the transform.
    transform_ = ComputeAffineTransform(guided_samples_, kCalibrationTargets);
    if (!transform_) {
      stage_ = Stage::kRejected;
      UpdateStatusLabel();
      return;
    }

    stage_ = Stage::kValidating;
    current_target_index_ = 0;
    UpdateStatusLabel();
    return;
  }

  // Stage::kValidating.
  const auto result = ValidateTap(*transform_, raw, kValidationTargets[static_cast<size_t>(current_target_index_)],
                                   max_error_px_);
  last_validation_error_px_ = result.error_px;
  if (!result.accepted) {
    stage_ = Stage::kRejected;
    UpdateStatusLabel();
    return;
  }

  ++current_target_index_;
  if (current_target_index_ >= static_cast<int>(kValidationTargets.size())) {
    stage_ = Stage::kAccepted;
  }
  UpdateStatusLabel();
}

void CalibrationFlow::Reset() {
  stage_ = Stage::kGuiding;
  current_target_index_ = 0;
  last_validation_error_px_ = 0;
  transform_.reset();
  guided_samples_ = {};
  UpdateStatusLabel();
}

}  // namespace firmware::ui::calibration
