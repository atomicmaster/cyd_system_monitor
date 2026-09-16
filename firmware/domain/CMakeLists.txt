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

set(_fd_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_fd_profile_toml
    "${CMAKE_CURRENT_SOURCE_DIR}/../../hardware/profiles/${FIRMWARE_DOMAIN_ACTIVE_PROFILE}/profile.toml")
set(_fd_profile_header "${_fd_generated_dir}/firmware/domain/generated/profile.hpp")
set(_fd_generator "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_profile_header.py")

if(ESP_PLATFORM)
  # idf_component_register validates that every INCLUDE_DIRS entry already
  # exists; the generator only fills it in during the build.
  file(MAKE_DIRECTORY "${_fd_generated_dir}")

  # Building as an ESP-IDF component under idf.py.
  idf_component_register(
    SRCS "src/geometry_check.cpp"
    INCLUDE_DIRS "include" "${_fd_generated_dir}"
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

  add_library(firmware_domain STATIC src/geometry_check.cpp)
  add_dependencies(firmware_domain firmware_domain_generated)
  target_include_directories(firmware_domain PUBLIC include "${_fd_generated_dir}")
  target_compile_features(firmware_domain PUBLIC cxx_std_20)
endif()
