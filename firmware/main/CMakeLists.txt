# SPDX-License-Identifier: Apache-2.0
idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "."
  REQUIRES domain ui esp32_platform lvgl esp_timer bt esp_wifi esp_event esp_netif
)
