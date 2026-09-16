// SPDX-License-Identifier: Apache-2.0
#include "microsd/microsd.hpp"

#include "driver/sdmmc_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "firmware/domain/generated/profile.hpp"
#include "sdmmc_cmd.h"

namespace firmware::platform::esp32::microsd {

namespace {

// PROVISIONAL: shares the expansion SPI bus's mosi/sclk/miso (see
// board_init.cpp), using MicroSD's own CS. This is SPI3 (VSPI), the
// ESP32's second general-purpose SPI controller, kept separate from the
// display's SPI2. Touch uses bit-banged software SPI instead of a third
// hardware controller (touch/touch.hpp), which is the provisional
// resolution to the board's three-buses-vs-two-controllers constraint.
constexpr spi_host_device_t kMicrosdSpiHost = SPI3_HOST;
constexpr const char* kMountPoint = "/sdcard";

bool g_bus_initialized = false;

}  // namespace

ProbeResult Probe() {
  ProbeResult result;

  if (!g_bus_initialized) {
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = firmware::domain::profile::kMicrosdMosi;
    bus_cfg.miso_io_num = firmware::domain::profile::kMicrosdMiso;
    bus_cfg.sclk_io_num = firmware::domain::profile::kMicrosdSclk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;
    if (spi_bus_initialize(kMicrosdSpiHost, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
      result.present = false;
      result.detail = "SPI bus init failed";
      return result;
    }
    g_bus_initialized = true;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = false;
  mount_cfg.max_files = 2;
  mount_cfg.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = kMicrosdSpiHost;

  sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_cfg.gpio_cs = static_cast<gpio_num_t>(firmware::domain::profile::kMicrosdCs);
  slot_cfg.host_id = kMicrosdSpiHost;

  sdmmc_card_t* card = nullptr;
  esp_err_t err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_cfg, &mount_cfg, &card);
  if (err != ESP_OK) {
    // Missing/unreadable SD is an ordinary idle status, not a failure.
    result.present = false;
    result.detail = (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) ? "not present (idle)"
                                                                         : "present but unreadable";
    return result;
  }

  result.present = true;
  result.detail = "mounted";
  esp_vfs_fat_sdcard_unmount(kMountPoint, card);
  return result;
}

}  // namespace firmware::platform::esp32::microsd
