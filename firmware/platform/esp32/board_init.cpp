// SPDX-License-Identifier: Apache-2.0
//
// Provisional bus arrangement (single documented decision point; see
// hardware/profiles/lcdwiki-esp32-32e-2.8/README.md's "Observation and
// feasibility limits" section for the underlying three-buses-vs-two-
// controllers question):
//
//   - Display: dedicated hardware SPI2 (HSPI). Never shared.
//   - MicroSD + expansion header: share hardware SPI3 (VSPI). They are not
//     used simultaneously in the MVP, so contending for the same
//     controller is acceptable for now.
//   - Touch (XPT2046): bit-banged/software GPIO SPI, not a hardware
//     controller at all, since only two hardware SPI controllers exist
//     and both are claimed above. See touch/touch.hpp.
//
// F08 measured this arrangement under combined load (display + touch +
// radio + MicroSD together, repeatedly, including a full session with a
// physical operator actively tapping the screen): it holds with no
// observed loss or degradation on any bus. An alternative (hardware-SPI
// touch, bit-banged MicroSD) was evaluated but deliberately not built --
// MicroSD's own hardware-SPI bandwidth only starts mattering once V2's
// SD-backed WiGLE Survey Artifacts writes to it; see
// hardware/profiles/lcdwiki-esp32-32e-2.8/development/feasibility.md's
// "Required next physical evidence" section for the reasoning and the
// combined-load evidence this decision rests on. Revisit before building
// that V2 feature, not before M2.
#include "board_init.hpp"

#include <string>

#include "audio/audio.hpp"
#include "battery/battery.hpp"
#include "build_identity.hpp"
#include "button/button.hpp"
#include "dev_console/dev_console.hpp"
#include "display/display.hpp"
#include "esp_log.h"
#include "expansion/expansion.hpp"
#include "firmware/domain/generated/profile.hpp"
#include "led/led.hpp"
#include "microsd/microsd.hpp"
#include "nvs_flash.h"
#include "reset_reason/reset_reason.hpp"
#include "touch/touch.hpp"

namespace firmware::platform::esp32::board {

namespace {

constexpr const char* kTag = "fw.board_init";

}  // namespace

firmware::domain::DiagnosticState InitBoard(BoardHandles& out_handles) {
  firmware::domain::DiagnosticState state;
  state.profile_id = firmware::domain::profile::kProfileId;
  state.build_identity = firmware::platform::esp32::kBuildIdentity;

  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  state.reset_reason = firmware::platform::esp32::reset_reason::ReadResetReason();

  out_handles.display = firmware::platform::esp32::display::InitDisplay();
  state.peripherals.push_back({"display", out_handles.display != nullptr,
                               out_handles.display != nullptr ? "initialized" : "init failed"});

  out_handles.touch_ready = out_handles.touch.Init();
  state.peripherals.push_back({"touch", out_handles.touch_ready,
                               out_handles.touch_ready
                                   ? "bit-banged SPI ready (PROVISIONAL bus, see F08)"
                                   : "init failed"});

  const auto sd_result = firmware::platform::esp32::microsd::Probe();
  state.peripherals.push_back({"microsd", sd_result.present, sd_result.detail});

  const bool led_ok = firmware::platform::esp32::led::Init();
  state.peripherals.push_back({"rgb_led", led_ok, led_ok ? "ready" : "init failed"});

  const bool audio_ok = firmware::platform::esp32::audio::Init();
  state.peripherals.push_back(
      {"audio", audio_ok,
       audio_ok ? "enabled pin ready, amplifier off (alert default off)" : "init failed"});

  const bool battery_ok = firmware::platform::esp32::battery::Init();
  std::string battery_detail = "init failed";
  if (battery_ok) {
    const int mv = firmware::platform::esp32::battery::ReadMillivolts();
    battery_detail = std::to_string(mv) + "mV (diagnostic only, no state of charge)";
  }
  state.peripherals.push_back({"battery", battery_ok, battery_detail});

  const bool button_ok = firmware::platform::esp32::button::Init();
  state.peripherals.push_back(
      {"boot_button", button_ok,
       button_ok ? (firmware::platform::esp32::button::IsPressed() ? "held" : "released")
                 : "init failed"});

  // Unconnected by default: a floating reading here isn't a pass/fail
  // result, just the pin's configured/idle state. F07 exercises this
  // properly by jumpering it to GND/3V3 and re-reading live via
  // DEV:PERIPHERAL_STATUS, not from this one-shot boot-time value.
  const bool expansion_ok = firmware::platform::esp32::expansion::Init();
  state.peripherals.push_back(
      {"expansion", expansion_ok,
       expansion_ok ? "configured, floating (jumper to GND/3V3 to test)" : "init failed"});

  firmware::platform::esp32::dev_console::StartDevConsole();

  ESP_LOGI(kTag, "board init complete: profile=%s build=%s reset_reason=%s",
           state.profile_id.c_str(), state.build_identity.c_str(),
           firmware::domain::ToString(state.reset_reason));

  return state;
}

}  // namespace firmware::platform::esp32::board
