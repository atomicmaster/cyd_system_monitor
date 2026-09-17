# SPDX-License-Identifier: Apache-2.0
#
# The actual ESP-IDF component registration for firmware/platform/esp32
# lives in this dedicated subdirectory, not in platform/esp32/ itself.
# idf_component_register's component name is always its containing
# directory's basename; registering directly from platform/esp32/ would
# name this component "esp32", colliding with ESP-IDF's own built-in
# "esp32" chip-support component (ld scripts, ROM bindings) that
# EXTRA_COMPONENT_DIRS entries take priority over and would silently
# shadow, breaking the build. Sources stay in their ticket-owned
# directories one level up (../display/, ../touch/, etc.); only the
# CMakeLists.txt/idf_component.yml registration point moved here.
idf_component_register(
  SRCS
    "../board_init.cpp"
    "../display/display.cpp"
    "../touch/touch.cpp"
    "../led/led.cpp"
    "../audio/audio.cpp"
    "../microsd/microsd.cpp"
    "../battery/battery.cpp"
    "../button/button.cpp"
    "../expansion/expansion.cpp"
    "../reset_reason/reset_reason.cpp"
    "../nvs/calibration_store.cpp"
    "../dev_console/dev_console.cpp"
    "../capacity/capacity_probe.cpp"
    "../capacity/cpu_load.cpp"
    "../radio/controller_probe.cpp"
  INCLUDE_DIRS ".."
  REQUIRES domain driver esp_lcd esp_adc sdmmc fatfs esp_timer nvs_flash lvgl esp_lcd_ili9341 bt esp_wifi esp_event esp_netif
)
