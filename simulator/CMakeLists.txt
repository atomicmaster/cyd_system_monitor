# SPDX-License-Identifier: GPL-3.0-only
#
# Desktop target linking the same firmware/domain and firmware/ui source as
# the ESP32 image, plus LVGL (pinned v9.2.2, F05) built headless: software
# renderer only, no SDL/windowing/X11 dependency, so this builds in CI and
# in sandboxes with no display.
cmake_minimum_required(VERSION 3.20)
project(cyd_system_monitor_simulator LANGUAGES CXX)

include(FetchContent)

set(LVGL_GIT_TAG "v9.2.2" CACHE STRING "LVGL tag pinned by F05")
# Keep build times reasonable: firmware/ui only needs basic widgets, not
# LVGL's own example/demo galleries.
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG "${LVGL_GIT_TAG}"
  GIT_SHALLOW TRUE
  # This LVGL version's plain (non-ESP-IDF) CMake path always defines
  # lvgl_demos/lvgl_examples targets with no option to gate them off.
  # EXCLUDE_FROM_ALL keeps them out of ninja's default build set since
  # nothing here links them, so build time stays reasonable.
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(lvgl)

# lv_conf_internal.h looks for "lv_conf.h" unless LV_CONF_PATH names one
# explicitly; point every lvgl translation unit (and anything linking it)
# at firmware/ui's shared minimal config via a PUBLIC definition.
target_compile_definitions(lvgl PUBLIC
  LV_CONF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/../firmware/ui/lv_conf.h)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../firmware/domain" firmware_domain)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../firmware/ui" firmware_ui)

add_executable(simulator
  src/main.cpp
  src/lvgl_env.cpp
  src/scenarios/display_smoke.cpp
  src/scenarios/calibration_scenario.cpp
  src/scenarios/peripherals_scenario.cpp
  src/scenarios/capacity_scenario.cpp
  src/fixtures.cpp)
target_link_libraries(simulator PRIVATE firmware_domain firmware_ui lvgl)
target_compile_features(simulator PRIVATE cxx_std_20)
