# SPDX-License-Identifier: Apache-2.0
#
# Proves the C++ consumer builds against generated output. `./dev check
# protocol` configures and runs this tree directly.
cmake_minimum_required(VERSION 3.20)
project(protocol_generated_tests LANGUAGES CXX)

add_library(protocol_generated INTERFACE)
target_include_directories(protocol_generated INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../generated/cpp")
target_compile_features(protocol_generated INTERFACE cxx_std_20)

add_library(protocol_core STATIC ../../core/cpp/protocol.cpp)
target_include_directories(protocol_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../core/cpp")
target_compile_features(protocol_core PUBLIC cxx_std_20)

add_executable(protocol_generated_tests test_smoke.cpp)
target_link_libraries(protocol_generated_tests PRIVATE protocol_generated)

add_executable(protocol_core_tests test_core.cpp)
target_link_libraries(protocol_core_tests PRIVATE protocol_core)
target_compile_definitions(protocol_core_tests PRIVATE
  CYD_PROTOCOL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures")

add_executable(protocol_parity_cli parity_cli.cpp)
target_link_libraries(protocol_parity_cli PRIVATE protocol_core)

enable_testing()
add_test(NAME protocol_generated_tests COMMAND protocol_generated_tests)
add_test(NAME protocol_core_tests COMMAND protocol_core_tests)
