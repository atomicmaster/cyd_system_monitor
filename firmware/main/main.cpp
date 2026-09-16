// SPDX-License-Identifier: Apache-2.0
//
// Boots the board, brings up all of firmware/platform/esp32's drivers, and
// pumps LVGL's timer handler to drive whichever screen is active: guided
// calibration on first boot (or after an incompatible/missing stored
// record), then the diagnostic screen (F05) once a valid calibration is
// loaded or a fresh calibration is accepted (F06). See
// docs/tickets/F05.md/F06.md/F07.md for what remains unverified pending
// physical hardware.
#include <cstdlib>
#include <optional>

#include "board_init.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "firmware/domain/calibration/calibration.hpp"
#include "firmware/domain/generated/profile.hpp"
#include "firmware/domain/profile_check.hpp"
#include "firmware/ui/calibration/calibration_flow.hpp"
#include "firmware/ui/diagnostic/diagnostic_screen.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs/calibration_store.hpp"

namespace {

namespace board = firmware::platform::esp32::board;
namespace nvs = firmware::platform::esp32::nvs;
namespace calibration = firmware::domain::calibration;
namespace ui_calibration = firmware::ui::calibration;
namespace ui_diagnostic = firmware::ui::diagnostic;

constexpr const char* kTag = "fw.main";
// The single orientation the MVP supports; see firmware/domain/geometry.hpp
// and profile.toml's display.default_orientation.
constexpr const char* kOrientation = "landscape";

// Everything app_main needs across loop iterations: the board handles from
// InitBoard, and exactly one of the two mutually-exclusive screens
// (calibration xor diagnostic) at a time.
struct AppState {
  board::BoardHandles board;
  firmware::domain::DiagnosticState diagnostic_state;

  std::optional<ui_calibration::CalibrationFlow> calibration;
  ui_diagnostic::DiagnosticScreenHandles diagnostic;
  bool touch_was_pressed = false;
};

AppState g_app;

void ShowDiagnosticScreen() {
  lv_obj_t* screen = lv_display_get_screen_active(g_app.board.display);
  g_app.diagnostic = ui_diagnostic::BuildDiagnosticScreen(screen, g_app.diagnostic_state);
  if (g_app.diagnostic.recalibrate_button != nullptr) {
    lv_obj_add_event_cb(
        g_app.diagnostic.recalibrate_button,
        [](lv_event_t*) {
          if (g_app.diagnostic.root != nullptr) {
            lv_obj_delete(g_app.diagnostic.root);
            g_app.diagnostic = {};
          }
          lv_obj_t* screen = lv_display_get_screen_active(g_app.board.display);
          g_app.calibration.emplace(screen);
          g_app.touch_was_pressed = false;
        },
        LV_EVENT_CLICKED, nullptr);
  }
}

void PersistAndShowDiagnostic(const calibration::AffineTransform& transform) {
  calibration::CalibrationRecord record;
  record.profile_id = firmware::domain::profile::kProfileId;
  record.orientation = kOrientation;
  record.schema_version = calibration::kCalibrationSchemaVersion;
  record.transform = transform;
  record.checksum = calibration::ComputeChecksum(record);
  if (!nvs::SaveCalibrationRecord(record)) {
    ESP_LOGE(
        kTag,
        "failed to persist calibration record; recalibration will be required again next boot");
  }

  if (g_app.calibration.has_value()) {
    lv_obj_delete(g_app.calibration->root());
    g_app.calibration.reset();
  }
  ShowDiagnosticScreen();
}

// Polls the touch driver once per loop iteration and drives whichever
// screen is active. Touch-down is edge-triggered (touch_was_pressed) so a
// single physical tap advances the flow exactly once, not once per poll
// while held. A tap anywhere after a rejection retries from target 0,
// rather than requiring a separate control.
void PumpTouch() {
  if (!g_app.board.touch_ready || !g_app.calibration.has_value()) {
    return;
  }

  calibration::RawTouchSample sample;
  const bool pressed = g_app.board.touch.ReadRaw(sample);
  const bool edge = pressed && !g_app.touch_was_pressed;
  g_app.touch_was_pressed = pressed;
  if (!edge) {
    return;
  }

  // Diagnostic pending real physical calibration evidence (see
  // docs/tickets/F06.md): confirms touch-down is actually reaching this
  // point and shows the raw controller reading in each report, since a
  // "no response" tap could mean the XPT2046 IRQ line never asserts, or it
  // asserts but PumpTouch/SubmitRawSample isn't reached, or it reaches here
  // with implausible raw values -- this log distinguishes all three.
  ESP_LOGI(kTag, "touch-down: raw_x=%ld raw_y=%ld stage=%d target=%d",
           static_cast<long>(sample.raw_x), static_cast<long>(sample.raw_y),
           static_cast<int>(g_app.calibration->stage()), g_app.calibration->current_target_index());

  if (g_app.calibration->stage() == ui_calibration::CalibrationFlow::Stage::kRejected) {
    g_app.calibration->Reset();
    return;
  }

  g_app.calibration->SubmitRawSample(sample);
  if (g_app.calibration->stage() == ui_calibration::CalibrationFlow::Stage::kAccepted) {
    PersistAndShowDiagnostic(*g_app.calibration->transform());
  }
}

// DIAGNOSTIC, pending physical F06 evidence (see docs/tickets/F06.md):
// logs the IRQ pin level and an unconditional (IRQ-independent) raw X/Y
// read once a second, regardless of calibration state. The first physical
// session found IRQ never asserts for any tap at any of the 5 targets --
// this isolates whether that's genuinely an IRQ-detection fault (raw X/Y
// stay flat/unchanging too, meaning the whole SPI data path never
// responds) or specifically an IRQ-pin fault (raw X/Y visibly move when
// touched despite IrqAsserted() staying false, meaning touch detection
// should switch to a Z-pressure/threshold method instead of relying on
// the hardware IRQ pin). Not gated by touch_ready's screen-mode checks so
// it logs during either screen. Remove once F06 lands a real fix.
void PumpTouchDiagnostics() {
  if (!g_app.board.touch_ready) {
    return;
  }

  static int64_t s_last_log_us = 0;
  const int64_t now_us = esp_timer_get_time();
  if (now_us - s_last_log_us < 1'000'000) {
    return;
  }
  s_last_log_us = now_us;

  const bool irq = g_app.board.touch.IrqAsserted();
  const calibration::RawTouchSample sample = g_app.board.touch.ReadRawIgnoringIrq();
  ESP_LOGI(kTag, "touch-diagnostic: irq_asserted=%d raw_x=%ld raw_y=%ld", irq,
           static_cast<long>(sample.raw_x), static_cast<long>(sample.raw_y));
}

// firmware/ui/lv_conf.h sets LV_TICK_CUSTOM 0, which means LVGL supplies no
// tick source of its own: the app must give it one via lv_tick_set_cb(),
// or lv_timer_handler()'s internal refresh-period timer never sees real
// elapsed time and the display stalls after its first draw (this is the
// exact ESP32 wrapper LVGL's own porting docs recommend: see
// docs/porting/tick.rst in the vendored lvgl source).
uint32_t TickGetMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

extern "C" void app_main(void) {
  // An unsupported profile geometry must fail clearly rather than boot
  // into an unvalidated layout.
  if (!firmware::domain::ValidateActiveProfile()) {
    abort();
  }

  lv_init();
  lv_tick_set_cb(TickGetMs);

  g_app.diagnostic_state = board::InitBoard(g_app.board);

  ESP_LOGI(kTag, "profile=%s build=%s reset_reason=%s", g_app.diagnostic_state.profile_id.c_str(),
           g_app.diagnostic_state.build_identity.c_str(),
           firmware::domain::ToString(g_app.diagnostic_state.reset_reason));

  if (g_app.board.display == nullptr) {
    ESP_LOGE(kTag, "display init failed; running headless (UART diagnostics and dev console only)");
  } else {
    lv_obj_t* screen = lv_display_get_screen_active(g_app.board.display);

    calibration::CalibrationRecord stored;
    const bool have_valid_calibration =
        g_app.board.touch_ready &&
        nvs::LoadCalibrationRecord(stored, firmware::domain::profile::kProfileId, kOrientation,
                                   calibration::kCalibrationSchemaVersion);

    if (have_valid_calibration || !g_app.board.touch_ready) {
      // No usable touch means calibration is unreachable; go straight to
      // diagnostics rather than presenting a flow that can never advance.
      ESP_LOGI(kTag, "boot: showing diagnostic screen (touch_ready=%d have_valid_calibration=%d)",
               g_app.board.touch_ready, have_valid_calibration);
      ShowDiagnosticScreen();
    } else {
      ESP_LOGI(kTag, "boot: entering guided calibration (no valid stored record)");
      g_app.calibration.emplace(screen);
    }
  }

  // Bit-banged touch polling happens once per loop iteration, not inside
  // LVGL's own timer/input-device framework (see PumpTouch's comment), so
  // it must never be starved by a long vTaskDelay. lv_timer_handler()
  // returns LV_NO_TIMER_READY (0xFFFFFFFF) once nothing is due to redraw
  // soon -- unclamped, pdMS_TO_TICKS() of that turns into an effectively
  // infinite delay, silently stalling touch polling (and this loop's own
  // diagnostic logging) the moment the screen finishes its first render
  // and goes idle. Clamp to a small bound so the loop always stays
  // touch-responsive regardless of what LVGL reports.
  constexpr uint32_t kMaxLoopSleepMs = 20;
  for (;;) {
    PumpTouch();
    PumpTouchDiagnostics();
    const uint32_t sleep_ms = lv_timer_handler();
    const uint32_t clamped_sleep_ms =
        (sleep_ms > 0 && sleep_ms < kMaxLoopSleepMs) ? sleep_ms : kMaxLoopSleepMs;
    vTaskDelay(pdMS_TO_TICKS(clamped_sleep_ms));
  }
}
