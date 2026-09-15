# SPDX-License-Identifier: Apache-2.0
#
# Proves the C++ consumer builds against generated output. `./dev check
# protocol` configures and runs this tree directly.
cmake_minimum_required(VERSION 3.20)
project(protocol_generated_tests LANGUAGES CXX)

add_library(protocol_generated INTERFACE)
target_include_directories(protocol_generated INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../generated/cpp")
target_compile_features(protocol_generated INTERFACE cxx_std_20)

add_executable(protocol_generated_tests test_smoke.cpp)
target_link_libraries(protocol_generated_tests PRIVATE protocol_generated)

enable_testing()
add_test(NAME protocol_generated_tests COMMAND protocol_generated_tests)
