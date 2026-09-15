# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{IDF_PATH})
  message(FATAL_ERROR
    "IDF_PATH is not set. Install ESP-IDF and run "
    "'source $IDF_PATH/export.sh' before invoking idf.py; see docs/development.md.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# domain/ registers itself as a component when ESP_PLATFORM is set.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}")

project(cyd_system_monitor_firmware)
