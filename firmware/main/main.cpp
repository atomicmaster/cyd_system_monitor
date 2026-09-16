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

  // The transform LVGL's pointer indev (see TouchIndevReadCb) maps raw
  // touch into screen coordinates with -- set once a calibration is known
  // good (loaded at boot or just accepted), cleared while re-calibrating.
  // Only meaningful once g_app.calibration has no value (the diagnostic
  // screen, where widgets like recalibrate_button need real click
  // detection); during CalibrationFlow itself, raw samples are fed
  // directly to SubmitRawSample() instead, deliberately bypassing this,
  // since establishing the transform is the whole point of that flow.
  std::optional<calibration::AffineTransform> active_transform;
};

AppState g_app;

// LVGL's own widget click/gesture detection (LV_EVENT_CLICKED, used by
// recalibrate_button below) only ever fires for input fed through a
// registered lv_indev_t -- it has no connection to PumpTouch()'s manual
// polling, which drives CalibrationFlow directly and never touches LVGL's
// indev system at all. Without this, no LVGL widget on any screen could
// ever receive a touch event. Registered once at boot; inert (always
// reports released) until active_transform has a value.
void TouchIndevReadCb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  static bool s_was_pressed = false;

  if (!g_app.board.touch_ready || !g_app.active_transform.has_value()) {
    data->state = LV_INDEV_STATE_RELEASED;
    s_was_pressed = false;
    return;
  }

  calibration::RawTouchSample raw;
  if (!g_app.board.touch.ReadRaw(raw)) {
    data->state = LV_INDEV_STATE_RELEASED;
    s_was_pressed = false;
    return;
  }

  const calibration::ScreenPoint point = calibration::ApplyTransform(*g_app.active_transform, raw);
  data->point.x = point.x;
  data->point.y = point.y;
  data->state = LV_INDEV_STATE_PRESSED;

  // DIAGNOSTIC, pending physical F06 evidence: logs only on the down-edge
  // (not every read while held) so a tap's calibrated landing point can be
  // read directly off the serial monitor and compared against
  // recalibrate_button's logged bounds below.
  if (!s_was_pressed) {
    ESP_LOGI(kTag, "indev press: raw_x=%ld raw_y=%ld -> screen_x=%ld screen_y=%ld",
             static_cast<long>(raw.raw_x), static_cast<long>(raw.raw_y), static_cast<long>(point.x),
             static_cast<long>(point.y));
  }
  s_was_pressed = true;
}

void ShowDiagnosticScreen() {
  lv_obj_t* screen = lv_display_get_screen_active(g_app.board.display);
  g_app.diagnostic = ui_diagnostic::BuildDiagnosticScreen(screen, g_app.diagnostic_state);
  if (g_app.diagnostic.recalibrate_button != nullptr) {
    // DIAGNOSTIC, pending physical F06 evidence: logs the button's actual
    // on-screen bounds once, to compare against "indev press: ... ->
    // screen_x/y" log lines and tell a coordinate-mapping problem (tap
    // lands outside these bounds) apart from LVGL never detecting the
    // press at all (button bounds look right, but neither this nor
    // LV_EVENT_CLICKED below ever logs).
    lv_obj_update_layout(g_app.diagnostic.recalibrate_button);
    ESP_LOGI(kTag, "recalibrate_button bounds: x=%ld y=%ld w=%ld h=%ld",
             static_cast<long>(lv_obj_get_x(g_app.diagnostic.recalibrate_button)),
             static_cast<long>(lv_obj_get_y(g_app.diagnostic.recalibrate_button)),
             static_cast<long>(lv_obj_get_width(g_app.diagnostic.recalibrate_button)),
             static_cast<long>(lv_obj_get_height(g_app.diagnostic.recalibrate_button)));

    lv_obj_add_event_cb(
        g_app.diagnostic.recalibrate_button,
        [](lv_event_t*) { ESP_LOGI(kTag, "recalibrate_button: LV_EVENT_PRESSED"); },
        LV_EVENT_PRESSED, nullptr);

    lv_obj_add_event_cb(
        g_app.diagnostic.recalibrate_button,
        [](lv_event_t*) {
          ESP_LOGI(kTag, "recalibrate_button: LV_EVENT_CLICKED");
          if (g_app.diagnostic.root != nullptr) {
            lv_obj_delete(g_app.diagnostic.root);
            g_app.diagnostic = {};
          }
          g_app.active_transform.reset();
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
  g_app.active_transform = transform;
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

  // lv_indev_create() binds the new device to lv_display_get_default() at
  // the moment it's called -- created before InitBoard() (which is where
  // the real lv_display_t comes from, deep inside display::InitDisplay())
  // permanently bound the indev to no display at all. LVGL logs a warning
  // for exactly this ("no display was created so far"), but LV_USE_LOG 0
  // in firmware/ui/lv_conf.h silently swallows it, which is why this went
  // unnoticed until physical testing showed the recalibrate button's
  // LV_EVENT_CLICKED (and even LV_EVENT_PRESSED) never firing despite
  // confirmed real touches. Must run after InitBoard() succeeds.
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, TouchIndevReadCb);

  ESP_LOGI(kTag, "profile=%s build=%s reset_reason=%s", g_app.diagnostic_state.profile_id.c_str(),
           g_app.diagnostic_state.build_identity.c_str(),
           firmware::domain::ToString(g_app.diagnostic_state.reset_reason));

  // F07 evidence: board_init.cpp's DiagnosticState.peripherals is normally
  // only ever seen on the diagnostic screen itself; logging it too means
  // LED/audio/microSD/battery/button results from the boot-time probe are
  // captured in UART evidence without needing a photo of that screen.
  for (const auto& peripheral : g_app.diagnostic_state.peripherals) {
    ESP_LOGI(kTag, "peripheral: %s present=%d detail=\"%s\"", peripheral.name.c_str(),
             peripheral.present, peripheral.detail.c_str());
  }

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
      if (have_valid_calibration) {
        g_app.active_transform = stored.transform;
      }
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
