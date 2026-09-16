# SPDX-License-Identifier: Apache-2.0
#
# firmware/ui holds LVGL screens shared by the ESP32 firmware image and the
# desktop simulator: LVGL headers only, no ESP-IDF headers, mirroring
# firmware/domain/CMakeLists.txt's dual-mode structure and its own
# documented ESP-IDF early-scan constraint (non-declarative commands must
# run after idf_component_register()).

set(_fu_sources
    "src/diagnostic/diagnostic_screen.cpp"
    "src/calibration/calibration_flow.cpp"
    "src/capacity/capacity_slice.cpp")

if(ESP_PLATFORM)
  # Building as an ESP-IDF component under idf.py. `lvgl` is the managed
  # component pinned in idf_component.yml (v9.2.2).
  idf_component_register(
    SRCS ${_fu_sources}
    INCLUDE_DIRS "include"
    REQUIRES domain lvgl
  )

  # LVGL's own lv_conf_internal.h looks for "lv_conf.h" unless LV_CONF_PATH
  # names one explicitly. Setting it as a build-wide property (rather than
  # only on this component) makes the lvgl component itself, which does not
  # depend on firmware/ui, also see the same minimal config.
  idf_build_set_property(COMPILE_DEFINITIONS
    "LV_CONF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" APPEND)
else()
  # Building standalone: desktop simulator. Depends on a target named
  # `lvgl`, provided by simulator/CMakeLists.txt via FetchContent, which
  # also sets LV_CONF_PATH as a PUBLIC compile definition on that target so
  # it propagates here.
  add_library(firmware_ui STATIC ${_fu_sources})
  target_include_directories(firmware_ui PUBLIC include)
  target_link_libraries(firmware_ui PUBLIC firmware_domain lvgl)
  target_compile_features(firmware_ui PUBLIC cxx_std_20)
endif()
