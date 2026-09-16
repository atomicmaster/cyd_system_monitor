# SPDX-License-Identifier: Apache-2.0
#
# Sources/includes for firmware::domain::calibration. This file is
# include()'d (not add_subdirectory()'d) by ../CMakeLists.txt, so it stays a
# plain set()-only fragment that is safe to evaluate during ESP-IDF's
# restricted early component-requirements scan, the same constraint
# ../CMakeLists.txt documents at its own top.
set(FIRMWARE_DOMAIN_CALIBRATION_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/calibration.cpp")
set(FIRMWARE_DOMAIN_CALIBRATION_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/include")
