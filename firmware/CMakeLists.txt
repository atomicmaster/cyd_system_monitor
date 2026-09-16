# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{IDF_PATH})
  message(FATAL_ERROR
    "IDF_PATH is not set. Install ESP-IDF and run "
    "'source $IDF_PATH/export.sh' before invoking idf.py; see docs/development.md.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# List each component's own directory, never CMAKE_CURRENT_LIST_DIR itself:
# that directory holds this project's own CMakeLists.txt, and ESP-IDF's
# component scanner treats a listed directory that already contains a
# CMakeLists.txt as one component, re-including this file during its early
# requirements scan. platform/esp32/ and ui/ joined this list in F05.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/domain"
    "${CMAKE_CURRENT_LIST_DIR}/ui"
    "${CMAKE_CURRENT_LIST_DIR}/platform/esp32/esp32_platform")

project(cyd_system_monitor_firmware)
