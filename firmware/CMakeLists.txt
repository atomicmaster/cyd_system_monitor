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
# requirements scan. platform/esp32/ and ui/ join this list once F05 gives
# them their own CMakeLists.txt.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/domain")

project(cyd_system_monitor_firmware)
