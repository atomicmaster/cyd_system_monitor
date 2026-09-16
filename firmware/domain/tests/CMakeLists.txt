# SPDX-License-Identifier: Apache-2.0
#
# Standalone root: proves firmware/domain builds and runs without ESP-IDF.
# `./dev check firmware` configures and runs this tree directly.
cmake_minimum_required(VERSION 3.20)
project(firmware_domain_tests LANGUAGES CXX)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." domain)

add_executable(firmware_domain_tests test_geometry.cpp)
target_link_libraries(firmware_domain_tests PRIVATE firmware_domain)

# One binary per domain module, mirroring the 1:1 domain/tests structure
# test_geometry.cpp established: each hand-rolled test binary owns one
# main() and prints its own ok/failure-count line.
add_executable(firmware_domain_calibration_tests test_calibration.cpp)
target_link_libraries(firmware_domain_calibration_tests PRIVATE firmware_domain)

add_executable(firmware_domain_diagnostic_tests test_diagnostic.cpp)
target_link_libraries(firmware_domain_diagnostic_tests PRIVATE firmware_domain)

enable_testing()
add_test(NAME firmware_domain_tests COMMAND firmware_domain_tests)
add_test(NAME firmware_domain_calibration_tests COMMAND firmware_domain_calibration_tests)
add_test(NAME firmware_domain_diagnostic_tests COMMAND firmware_domain_diagnostic_tests)
