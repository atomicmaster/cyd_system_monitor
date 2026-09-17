<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# F08 investigation: passive Wi-Fi monitor capacity

**Status: candidate for a measured capacity probe, not a demonstrated memory
saving.** This note applies only to the original ESP32 and ESP-IDF **v5.3.2**
used by the `lcdwiki-esp32-32e-2.8` profile. It does not make an operational
claim about reception completeness.

> **Superseded (2026-09-17).** The 376 B DRAM recovery recorded here was
> measured against a build whose real constraint was two static buffers
> competing for the ESP32's single linkable DRAM segment. Once the LVGL pool
> and display draw buffer moved to the runtime heap, DRAM stopped being the
> binding limit entirely. The passive-monitor API path and its Wi-Fi IRAM
> options below are still accurate and still used; only the capacity framing
> is obsolete. See
> [the feasibility record](feasibility.md#capacity-blocker-resolved-two-measurement-errors).

## Passive monitor API path

ESP-IDF's own v5.3.2 simple-sniffer example initializes Wi-Fi, selects
`WIFI_STORAGE_RAM`, and sets `WIFI_MODE_NULL`. Its sniffer then starts Wi-Fi,
installs a promiscuous callback and filter, enables promiscuous mode, and sets
a fixed channel. That is direct evidence that `WIFI_MODE_NULL` is a supported
mode for passive promiscuous capture; it does not need a STA association,
SoftAP, IP stack, scan result list, or application data transmission.

The minimal runtime sequence is therefore:

1. `esp_wifi_init(&WIFI_INIT_CONFIG_DEFAULT())`;
2. `esp_wifi_set_storage(WIFI_STORAGE_RAM)` and
   `esp_wifi_set_mode(WIFI_MODE_NULL)`;
3. `esp_wifi_start()`;
4. call `esp_wifi_set_promiscuous_filter()` with only the frame categories the
   product consumes, install `esp_wifi_set_promiscuous_rx_cb()`, enable with
   `esp_wifi_set_promiscuous(true)`, then select the listening channel with
   `esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE)`.

The callback is still on the Wi-Fi receive path: it should count and copy only
the bounded metadata/product bytes needed by F08, then hand off through a
bounded queue. It must not parse, retain, log, or draw arbitrary packets in
that callback.

Sources:

- [ESP-IDF v5.3.2 simple-sniffer Wi-Fi initialization](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/network/simple_sniffer/main/simple_sniffer_example_main.c#L84-L90)
- [ESP-IDF v5.3.2 simple-sniffer promiscuous start sequence](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/network/simple_sniffer/main/cmd_sniffer.c#L285-L293)
- [ESP-IDF v5.3.2 Wi-Fi API: promiscuous callback, enable, and filters](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/include/esp_wifi.h#L851-L960)
- [ESP-IDF v5.3.2 Wi-Fi API: channel may be set after start](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/include/esp_wifi.h#L744-L765)
- [ESP-IDF v5.3.2 promiscuous packet filter masks](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/include/esp_wifi_types_generic.h#L568-L575)

`WIFI_PROMIS_FILTER_MASK_MGMT` is the conservative starting filter for beacon
and management-frame observation. Add `WIFI_PROMIS_FILTER_MASK_DATA` only if
the product requirement actually needs data frames; data in promiscuous mode
can be encrypted, and expanding the filter increases callback load. The frame
filter is a runtime selectivity tool, not proof that the underlying driver can
allocate fewer receive buffers.

## Capacity that can be reclaimed safely

The following features exist to establish, serve, secure, or optimize a Wi-Fi
connection. They are not prerequisites of the `WIFI_MODE_NULL` + promiscuous
API path above, so disabling them is a sound *candidate* for the probe:

| Setting | Candidate | Why it does not change passive receive | Important boundary |
| --- | --- | --- | --- |
| SoftAP | `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n` | No AP is created in null mode. | Removes the ability to serve/configure through SoftAP. ESP-IDF states it can be compiled out to save code size. |
| Wi-Fi NVS | `CONFIG_ESP_WIFI_NVS_ENABLED=n` | The selected channel is not persisted and the monitor explicitly uses RAM storage. | PHY calibration / project NVS may still be initialized separately; do not infer that all NVS is removable. |
| WPA3, SAE-PK, SoftAP SAE, OWE | all `n` | No association or authentication occurs. | No WPA3/OWE connection support remains. |
| Enterprise and its TLS client | both `n` | No enterprise connection occurs. | Enterprise APIs become meaningless; this can remove binary code. |
| SoftAP beacon allocation | leave default; irrelevant once SoftAP is disabled | No SoftAP exists. | Do not attempt to use this as a standalone RAM saving. |
| AMPDU TX | `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n` | The monitor deliberately does not transmit application traffic. | Do not use this image for normal throughput or association. |
| Dynamic TX buffers | dynamic selected, count `1` | The v5.3.2 Kconfig minimum is one; TX buffer counts exist for frames delivered to the driver from TCP/IP. | It is unsafe to assert the controller never transmits any management/control frame; use the documented minimum, not zero. |
| Wi-Fi IRAM speed options | `ESP_WIFI_IRAM_OPT=n`, `ESP_WIFI_RX_IRAM_OPT=n` | They trade receive/throughput performance for IRAM, rather than enable the feature. | The official Kconfig says disabling them saves more than 10 KB and 17 KB of IRAM respectively, but receive performance must be physically measured. |

Sources:

- [ESP-IDF v5.3.2 Wi-Fi Kconfig: buffers and allocation lifetimes](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L27-L134)
- [ESP-IDF v5.3.2 Wi-Fi Kconfig: AMPDU, NVS, and management short buffers](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L176-L276)
- [ESP-IDF v5.3.2 Wi-Fi Kconfig: IRAM savings and connection-security options](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L302-L334)
- [ESP-IDF v5.3.2 Wi-Fi Kconfig: SoftAP can be compiled out](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L416-L422)
- [ESP-IDF v5.3.2 Wi-Fi Kconfig: enterprise reduction](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L726-L748)

## Do not prematurely cut these receive resources

Static RX buffers are allocated at `esp_wifi_init()` and are used by Wi-Fi
hardware to receive **all** 802.11 frames; each is approximately 1.6 KB. The
v5.3.2 minimum is two. Dynamic RX buffers deliver received frames to higher
layers; their documented limit must be at least the static RX-buffer count.
For the first passive probe, use the two/two minimum and prove acceptable loss
under the chosen filter and channel schedule.

Do **not** call `CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n` “safe” based only on this
configuration review. ESP-IDF exposes separate promiscuous filters for MPDU
and AMPDU data and describes AMPDU RX as a performance/compatibility feature.
Disabling it may be acceptable for a management-only monitor, but this must be
validated against the exact captured packet class and loss/revisit criteria.
Leave it enabled in the first conservative passive-monitor overlay; if the
link remains short, test that one change independently and record its capture
effect.

Likewise, use at least one static RX-management buffer. Although promiscuous
filtering can restrict what the application receives, it is not documentation
that a zero-sized management path works. The v5.3.2 Kconfig allows one to ten
such buffers; choose static allocation first to avoid fragmentation.

Sources:

- [ESP-IDF v5.3.2 Wi-Fi Kconfig: static and dynamic RX constraints](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L27-L70)
- [ESP-IDF v5.3.2 Wi-Fi Kconfig: RX-management buffer minimum and allocation guidance](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L144-L170)
- [ESP-IDF v5.3.2 Wi-Fi Kconfig: AMPDU RX trade-off](https://github.com/espressif/esp-idf/blob/v5.3.2/components/esp_wifi/Kconfig#L195-L217)

## Conservative probe overlay

Apply the following alongside the existing project defaults only for an
isolated capacity build. It preserves an AMPDU-capable receive path while
removing connection, security, SoftAP, and throughput-oriented capacity. The
configuration is intentionally not yet product policy.

```ini
# F08 passive Wi-Fi monitor probe; merge after firmware/sdkconfig.defaults.
CONFIG_ESP_WIFI_ENABLED=y

# Minimum documented receive allocations: two static, dynamic >= static.
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=2
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=2
CONFIG_ESP_WIFI_STATIC_RX_MGMT_BUFFER=y
# CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER is not set
CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF=1

# No application transmit path. Dynamic TX's documented minimum is one.
# CONFIG_ESP_WIFI_STATIC_TX_BUFFER is not set
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=1
CONFIG_ESP_WIFI_MGMT_SBUF_NUM=6
# CONFIG_ESP_WIFI_AMPDU_TX_ENABLED is not set

# Retain AMPDU RX until its effect on the required monitor frames is measured.
CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=y
CONFIG_ESP_WIFI_RX_BA_WIN=2

# No persistence, SoftAP, association security, or enterprise support.
# CONFIG_ESP_WIFI_NVS_ENABLED is not set
# CONFIG_ESP_WIFI_SOFTAP_SUPPORT is not set
# CONFIG_ESP_WIFI_ENABLE_WPA3_SAE is not set
# CONFIG_ESP_WIFI_ENABLE_SAE_PK is not set
# CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT is not set
# CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA is not set
# CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT is not set
# CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT is not set

# Reclaim IRAM; benchmark passive capture loss and UI latency on hardware.
# CONFIG_ESP_WIFI_IRAM_OPT is not set
# CONFIG_ESP_WIFI_EXTRA_IRAM_OPT is not set
# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set
```

The overlay does not remove the Wi-Fi driver, RF/PHY, its task, ESP32
Wi-Fi/Bluetooth coexistence, static RX buffers, or controller-level radio
time. It can only be accepted after a separate `idf.py size` comparison and a
physical F08 schedule records receiver-available time, channel dwell/revisit,
callback/queue drops, malformed frames, UI/touch latency, and the effect on
BLE scan windows. A build that links without these measurements remains only a
capacity candidate.

## Measured link result

On 2026-09-16, the conservative overlay above was combined with
`sdkconfig.defaults` and `sdkconfig.controller_only_probe.defaults`:

```sh
idf.py -B build-wifi-min \
  -D SDKCONFIG=build-wifi-min/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.controller_only_probe.defaults;sdkconfig.passive_wifi_probe.defaults' \
  build
```

The resolved `sdkconfig` confirmed the two/two static/dynamic RX minimum,
one management RX buffer, AMPDU RX enabled, one dynamic TX buffer, and all
listed connection/security and IRAM options disabled. The linker rejected the
image with **DRAM overflow of 12,632 B**. It reported no IRAM overflow.

For the same controller-only BLE probe with the ordinary Wi-Fi configuration,
DRAM overflow was 13,008 B and IRAM overflow was 2,140 B. Therefore this
passive Wi-Fi configuration recovered 376 B of link-time DRAM and at least
2,140 B of IRAM. It did not create a flashable image, so no board capture,
receive-loss, coexistence, or UI-latency claim was made. Wi-Fi trimming alone
cannot close the remaining DRAM gap.

## Follow-up: LVGL pool capacity probe

The UI's normal 64 KiB LVGL pool was made a build-time parameter whose
production default remains 64 KiB. A radio-only capacity build selected
32 KiB with `-D FIRMWARE_LV_MEM_SIZE_KIB=32` alongside this Wi-Fi overlay and
the controller-only BLE overlay. It linked with 20,140 B of static DRAM and
23,693 B of IRAM remaining. The 32 KiB choice is a capacity candidate based
on F08a's measured 10,908 B LVGL peak; it is not acceptance of the complete
future UI or combined runtime load.

The binary then exceeded the current 1 MiB factory-app partition by 95,216 B,
so it was not flashed. The next experiment must establish a suitable
partition layout (the profile has 2 MiB flash) or reduce image flash before
on-board radio and UI measurements can begin.

Applying `sdkconfig.large_app_probe.defaults` selected ESP-IDF's documented
1.5 MiB single-app/no-OTA partition layout. The same image then completed the
partition check with 389,136 B (26%) free. It is therefore eligible for a
physical runtime probe; this does not establish that its static DRAM margin
or passive reception quality is sufficient.

## First physical probe

On 2026-09-17, the combined probe at commit `6344ddc` was flashed to the
E32R28T on `/dev/cu.usbserial-140`. Boot confirmed `wifi:mode : null` and
sniffer enablement. Settled `DEV:HCI_STATUS` samples rose from 136 to 197
Wi-Fi management frames in twelve seconds, alongside BLE advertising reports
rising from 1,033 to 1,479; at about 96 seconds uptime they were 450 and
3,754 respectively. `dropped_events` and `malformed_events` stayed zero.

The result establishes live receive callbacks for both radio paths, not frame
capture completeness or radio coexistence quality. A future scheduled-window
experiment must measure channel dwell/revisit, receiver-available time, and
loss against a controlled transmitter before accepting F08 coverage.
