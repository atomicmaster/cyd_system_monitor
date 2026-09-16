// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/calibration/calibration_flow.hpp"

namespace firmware::ui::calibration {

using firmware::domain::calibration::ComputeAffineTransform;
using firmware::domain::calibration::kCalibrationTargets;
using firmware::domain::calibration::ScreenPoint;
using firmware::domain::calibration::ValidateTap;

namespace {

// Crosshair proportions inside a kTargetMarkerSizePx square, all centered
// on the square's own center: a border-only ring, a small filled center
// dot, and four arms extending from the ring's outer edge to the square's
// edge. Sized for easy precision tapping on a 320x240 panel without
// crowding adjacent targets (closest pair is 40px apart on an axis, e.g.
// the calibration corners to the panel edge inset).
constexpr lv_coord_t kRingDiameterPx = 16;
constexpr lv_coord_t kCenterDotDiameterPx = 5;
constexpr lv_coord_t kArmThicknessPx = 2;
constexpr lv_coord_t kArmLengthPx = (kTargetMarkerSizePx - kRingDiameterPx) / 2;

lv_obj_t* MakeArm(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                  lv_color_t color) {
  lv_obj_t* arm = lv_obj_create(parent);
  lv_obj_set_pos(arm, x, y);
  lv_obj_set_size(arm, w, h);
  lv_obj_set_style_radius(arm, 0, 0);
  lv_obj_set_style_bg_color(arm, color, 0);
  lv_obj_set_style_bg_opa(arm, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(arm, 0, 0);
  lv_obj_remove_flag(arm, LV_OBJ_FLAG_SCROLLABLE);
  return arm;
}

// Builds the crosshair (ring + center dot + four arms) as children of a
// kTargetMarkerSizePx square container, so CalibrationFlow only ever has
// to reposition one object (the container) to move the whole marker.
lv_obj_t* BuildCrosshairMarker(lv_obj_t* parent) {
  const lv_color_t color = lv_palette_main(LV_PALETTE_TEAL);
  constexpr lv_coord_t kCenter = kTargetMarkerSizePx / 2;

  lv_obj_t* container = lv_obj_create(parent);
  lv_obj_set_size(container, kTargetMarkerSizePx, kTargetMarkerSizePx);
  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(container, 0, 0);
  lv_obj_set_style_pad_all(container, 0, 0);
  lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* ring = lv_obj_create(container);
  lv_obj_set_pos(ring, kCenter - kRingDiameterPx / 2, kCenter - kRingDiameterPx / 2);
  lv_obj_set_size(ring, kRingDiameterPx, kRingDiameterPx);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ring, kArmThicknessPx, 0);
  lv_obj_set_style_border_color(ring, color, 0);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dot = lv_obj_create(container);
  lv_obj_set_pos(dot, kCenter - kCenterDotDiameterPx / 2, kCenter - kCenterDotDiameterPx / 2);
  lv_obj_set_size(dot, kCenterDotDiameterPx, kCenterDotDiameterPx);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, color, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

  // Top, bottom, left, right arms, each spanning from the container edge
  // to the ring's outer edge.
  MakeArm(container, kCenter - kArmThicknessPx / 2, 0, kArmThicknessPx, kArmLengthPx, color);
  MakeArm(container, kCenter - kArmThicknessPx / 2, kTargetMarkerSizePx - kArmLengthPx,
          kArmThicknessPx, kArmLengthPx, color);
  MakeArm(container, 0, kCenter - kArmThicknessPx / 2, kArmLengthPx, kArmThicknessPx, color);
  MakeArm(container, kTargetMarkerSizePx - kArmLengthPx, kCenter - kArmThicknessPx / 2,
          kArmLengthPx, kArmThicknessPx, color);

  return container;
}

}  // namespace

CalibrationFlow::CalibrationFlow(lv_obj_t* parent, double max_error_px)
    : max_error_px_(max_error_px) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 320, 240);
  // Zero padding/border so target_marker_'s coordinates map 1:1 onto the
  // same screen-pixel space firmware::domain::calibration's targets are
  // defined in -- LVGL's default container style otherwise insets the
  // content area, which would tap-offset every marker from its intended
  // calibration point.
  lv_obj_set_style_pad_all(root_, 0, 0);
  lv_obj_set_style_border_width(root_, 0, 0);

  // Title sits at the vertical center of the screen's top half (y=60 of
  // 240) and status at the center of the bottom half (y=180): both clear
  // of every guided/validation target point by at least 40px (the closest
  // is the center calibration target at y=120, and the top/bottom-mid
  // validation targets at y=20/y=220), so on-screen text never sits under
  // a marker the operator is trying to tap.
  title_label_ = lv_label_create(root_);
  lv_label_set_text(title_label_, "Touch Screen Calibration");
  lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 60 - 8);

  status_label_ = lv_label_create(root_);
  lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 180 - 8);

  target_marker_ = BuildCrosshairMarker(root_);

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
  UpdateTargetMarker();
}

void CalibrationFlow::UpdateTargetMarker() {
  if (stage_ == Stage::kAccepted || stage_ == Stage::kRejected) {
    lv_obj_add_flag(target_marker_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(target_marker_, LV_OBJ_FLAG_HIDDEN);

  const ScreenPoint point = (stage_ == Stage::kGuiding)
                                ? kCalibrationTargets[static_cast<size_t>(current_target_index_)]
                                : kValidationTargets[static_cast<size_t>(current_target_index_)];
  // target_marker_'s position is its top-left corner; center the marker on
  // the target point rather than anchoring a corner to it.
  lv_obj_set_pos(target_marker_, point.x - kTargetMarkerSizePx / 2,
                 point.y - kTargetMarkerSizePx / 2);
  // lv_obj_set_pos() only schedules a coordinate recompute; lv_obj_get_x/y
  // read the cached obj->coords directly and would otherwise see the
  // previous position until the next full layout/render pass. Flushing it
  // here keeps target_marker()'s position query-consistent immediately
  // after every SubmitRawSample()/Reset(), not just once LVGL gets around
  // to its own refresh timer.
  lv_obj_update_layout(target_marker_);
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
  const auto result =
      ValidateTap(*transform_, raw, kValidationTargets[static_cast<size_t>(current_target_index_)],
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
