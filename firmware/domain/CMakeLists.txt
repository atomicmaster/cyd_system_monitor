# SPDX-License-Identifier: Apache-2.0
#
# firmware/domain is pure C++: no ESP-IDF headers, so it links into the
# ESP32 firmware image (as an IDF component, when ESP_PLATFORM is set by
# idf.py), the desktop simulator, and standalone native tests.
#
# ESP-IDF scans every component's CMakeLists.txt in a restricted early pass
# to collect REQUIRES before idf_component_register() runs. Non-declarative
# commands (find_package, add_custom_command, ...) placed before that call
# break that pass ("define_property command is not scriptable"), so under
# ESP_PLATFORM everything but plain set() runs after registration.

set(FIRMWARE_DOMAIN_ACTIVE_PROFILE
    "lcdwiki-esp32-32e-2.8"
    CACHE STRING "Hardware profile id under hardware/profiles/ to validate against")

# CMAKE_CURRENT_LIST_DIR, not CMAKE_CURRENT_SOURCE_DIR, for every path
# computed here: ESP-IDF's early requirements-scan pass evaluates this file
# via a throwaway include() (not add_subdirectory()), which leaves
# CMAKE_CURRENT_SOURCE_DIR pointing at the top build directory rather than
# this component's own directory. CMAKE_CURRENT_LIST_DIR tracks the file
# actually being processed regardless of how it was reached, so it stays
# correct in both that early pass and the later real configure pass.
set(_fd_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_fd_profile_toml
    "${CMAKE_CURRENT_LIST_DIR}/../../hardware/profiles/${FIRMWARE_DOMAIN_ACTIVE_PROFILE}/profile.toml")
set(_fd_profile_header "${_fd_generated_dir}/firmware/domain/generated/profile.hpp")
set(_fd_generator "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_profile_header.py")

# calibration/CMakeLists.txt is a plain set()-only fragment (see its own
# comment); include() it rather than add_subdirectory() it so it shares
# this file's scope and early-scan safety.
include("${CMAKE_CURRENT_LIST_DIR}/calibration/CMakeLists.txt")

set(_fd_sources
    "src/geometry_check.cpp"
    "src/reset_reason.cpp"
    "src/diagnostic.cpp"
    "src/feasibility.cpp"
    "src/capacity.cpp"
    "${FIRMWARE_DOMAIN_CALIBRATION_SOURCES}")

if(ESP_PLATFORM)
  # idf_component_register validates that every INCLUDE_DIRS entry already
  # exists; the generator only fills it in during the build.
  file(MAKE_DIRECTORY "${_fd_generated_dir}")

  # Building as an ESP-IDF component under idf.py.
  idf_component_register(
    SRCS ${_fd_sources}
    INCLUDE_DIRS "include" "${FIRMWARE_DOMAIN_CALIBRATION_INCLUDE_DIR}" "${_fd_generated_dir}"
  )

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_custom_command(
    OUTPUT "${_fd_profile_header}"
    COMMAND "${Python3_EXECUTABLE}" "${_fd_generator}" "${_fd_profile_toml}" "${_fd_profile_header}"
    DEPENDS "${_fd_generator}" "${_fd_profile_toml}"
    COMMENT "Generating profile header from ${FIRMWARE_DOMAIN_ACTIVE_PROFILE}")
  add_custom_target(firmware_domain_generated DEPENDS "${_fd_profile_header}")
  add_dependencies(${COMPONENT_LIB} firmware_domain_generated)
else()
  # Building standalone: desktop simulator or native domain tests.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_custom_command(
    OUTPUT "${_fd_profile_header}"
    COMMAND "${Python3_EXECUTABLE}" "${_fd_generator}" "${_fd_profile_toml}" "${_fd_profile_header}"
    DEPENDS "${_fd_generator}" "${_fd_profile_toml}"
    COMMENT "Generating profile header from ${FIRMWARE_DOMAIN_ACTIVE_PROFILE}")
  add_custom_target(firmware_domain_generated DEPENDS "${_fd_profile_header}")

  add_library(firmware_domain STATIC ${_fd_sources})
  add_dependencies(firmware_domain firmware_domain_generated)
  target_include_directories(firmware_domain PUBLIC include
                              "${FIRMWARE_DOMAIN_CALIBRATION_INCLUDE_DIR}" "${_fd_generated_dir}")
  target_compile_features(firmware_domain PUBLIC cxx_std_20)
endif()
