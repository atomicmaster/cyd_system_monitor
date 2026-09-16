// SPDX-License-Identifier: GPL-3.0-only
#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/calibration/calibration.hpp"
#include "firmware/ui/calibration/calibration_flow.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {

namespace {

using firmware::domain::calibration::kCalibrationTargets;
using firmware::domain::calibration::RawTouchSample;
using firmware::domain::calibration::ScreenPoint;
using firmware::ui::calibration::CalibrationFlow;
using firmware::ui::calibration::kTargetMarkerSizePx;
using firmware::ui::calibration::kValidationTargets;

// The marker's position is its top-left corner; it's centered on `point`.
bool MarkerIsAt(const CalibrationFlow& flow, ScreenPoint point) {
  return lv_obj_get_x(flow.target_marker()) == point.x - kTargetMarkerSizePx / 2 &&
         lv_obj_get_y(flow.target_marker()) == point.y - kTargetMarkerSizePx / 2;
}

// Every raw sample here is target/2, the same noiseless known-transform
// fixture firmware/domain/tests/test_calibration.cpp uses (a=2,e=2): all
// five calibration corners/center and all four validation edge-midpoints
// have even coordinates, so raw samples land on exact integers with no
// rounding.
RawTouchSample HalfScale(firmware::domain::calibration::ScreenPoint target) {
  return RawTouchSample{target.x / 2, target.y / 2};
}

}  // namespace

int RunCalibration() {
  ScenarioResult result;
  lv_obj_t* screen = InitLvglHeadless();

  // Case 1: full guided calibration + validation pass, all taps accurate.
  {
    CalibrationFlow flow(screen);
    result.Expect(flow.stage() == CalibrationFlow::Stage::kGuiding,
                  "calibration flow starts in the guiding stage");
    result.Expect(!lv_obj_has_flag(flow.target_marker(), LV_OBJ_FLAG_HIDDEN),
                  "target marker is visible while guiding");
    result.Expect(MarkerIsAt(flow, kCalibrationTargets[0]),
                  "target marker starts centered on the first calibration target");

    for (size_t i = 0; i < kCalibrationTargets.size(); ++i) {
      flow.SubmitRawSample(HalfScale(kCalibrationTargets[i]));
      const bool last = i + 1 == kCalibrationTargets.size();
      if (!last) {
        result.Expect(MarkerIsAt(flow, kCalibrationTargets[i + 1]),
                      "target marker follows the guided target index after each tap");
      }
    }
    result.Expect(flow.stage() == CalibrationFlow::Stage::kValidating,
                  "flow advances to validating after the 5th guided sample");
    result.Expect(flow.transform().has_value(),
                  "a transform is computed from the 5 noiseless guided samples");
    result.Expect(MarkerIsAt(flow, kValidationTargets[0]),
                  "target marker moves to the first validation target on entering kValidating");

    for (size_t i = 0; i < kValidationTargets.size(); ++i) {
      flow.SubmitRawSample(HalfScale(kValidationTargets[i]));
      const bool last = i + 1 == kValidationTargets.size();
      if (!last) {
        result.Expect(MarkerIsAt(flow, kValidationTargets[i + 1]),
                      "target marker follows the validation target index after each tap");
      }
    }
    result.Expect(flow.stage() == CalibrationFlow::Stage::kAccepted,
                  "flow accepts calibration after every validation tap lands within tolerance");
    result.Expect(lv_obj_has_flag(flow.target_marker(), LV_OBJ_FLAG_HIDDEN),
                  "target marker is hidden once calibration is accepted");
  }

  // Case 2: degenerate guided input is rejected outright (no transform).
  {
    CalibrationFlow flow(screen);
    for (int i = 0; i < 5; ++i) {
      flow.SubmitRawSample(RawTouchSample{10, 10});  // coincident: degenerate
    }
    result.Expect(flow.stage() == CalibrationFlow::Stage::kRejected,
                  "degenerate guided samples reject calibration instead of producing a transform");
    result.Expect(!flow.transform().has_value(), "no transform is retained after a degenerate fit");
    result.Expect(lv_obj_has_flag(flow.target_marker(), LV_OBJ_FLAG_HIDDEN),
                  "target marker is hidden once calibration is rejected");

    // Recovery: Reset() returns to guiding target 0 so the operator can
    // retry, per F06's acceptance line about recoverable board operation.
    flow.Reset();
    result.Expect(flow.stage() == CalibrationFlow::Stage::kGuiding,
                  "Reset() returns a rejected flow to the guiding stage");
    result.Expect(flow.current_target_index() == 0, "Reset() returns to the first target");
    result.Expect(MarkerIsAt(flow, kCalibrationTargets[0]),
                  "target marker reappears on the first target after Reset()");
  }

  // Case 3: a deliberate validation-failure case -- guided calibration
  // succeeds, but one validation tap lands far outside max_error_px, so
  // the flow must reject rather than silently accepting bad coordinates.
  {
    CalibrationFlow flow(screen);
    for (size_t i = 0; i < kCalibrationTargets.size(); ++i) {
      flow.SubmitRawSample(HalfScale(kCalibrationTargets[i]));
    }
    result.Expect(flow.stage() == CalibrationFlow::Stage::kValidating,
                  "case 3 flow reaches validating with a valid transform");

    // First validation tap is wildly off target.
    flow.SubmitRawSample(RawTouchSample{0, 0});
    result.Expect(flow.stage() == CalibrationFlow::Stage::kRejected,
                  "an inaccurate validation tap is rejected rather than accepted");
    result.Expect(flow.last_validation_error_px() > flow.max_error_px(),
                  "the rejected tap's recorded error exceeds the configured limit");
  }

  return result.Finish("simulator calibration");
}

}  // namespace simulator::scenarios
