# SPDX-License-Identifier: GPL-3.0-only
#
# Desktop target linking the same firmware/domain source as the ESP32 image.
# LVGL rendering is wired in starting with F05/TB01, once firmware/ui has
# screens to show; M0 proves the shared domain link and a runnable smoke
# scenario with a controllable clock.
cmake_minimum_required(VERSION 3.20)
project(cyd_system_monitor_simulator LANGUAGES CXX)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../firmware/domain" firmware_domain)

add_executable(simulator src/main.cpp)
target_link_libraries(simulator PRIVATE firmware_domain)
target_compile_features(simulator PRIVATE cxx_std_20)
