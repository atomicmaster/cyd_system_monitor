<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# LCDWiki 2.8-inch ESP32-32E display

This profile describes LCDWiki's E32R28T and E32N28T boards. The variants share the ESP32-WROOM-32E, 240×320 ILI9341V display, onboard peripherals, and pin assignment. E32R28T adds an XPT2046 resistive touch screen; E32N28T has no touch screen. The first release officially supports only the connected and verified E32R28T touch variant; the E32N28T description is reference material rather than a support claim.

Source: [LCDWiki product page](https://www.lcdwiki.com/2.8inch_ESP32-32E_Display), retrieved September 15, 2026.

## Connected unit

Read-only probing on September 15, 2026 established:

- USB serial bridge: CH340/CH341 family, USB VID:PID `1a86:7523`
- macOS device node at probe time: `/dev/cu.usbserial-140` (the suffix and path may change)
- chip: ESP32-D0WD-V3, revision 3.1, dual core, 240 MHz
- crystal: 40 MHz
- SPI flash: 4 MB at 3.3 V

The connected unit has been visually identified as the **E32R28T touch variant**. Its XPT2046 resistive touch input must be calibrated on first start before normal touch navigation is enabled.

The unit's factory radio address is deliberately omitted because it is not needed to select or build the profile.

## Observation and feasibility limits

The module supports 2.4 GHz WiFi and BLE; it cannot capture 5/6 GHz WiFi. Host Metrics still arrive over USB on those bands, but Host-targeted radio correlation needs relevant captured evidence. See the [ESP32-WROOM-32E datasheet](https://documentation.espressif.com/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.html).

M1a validates the combined radio/display/touch/USB workload and establishes memory and flash budgets before the full Host application and UI work. Display, touch, and MicroSD have separate signal buses on this board, while ESP32 has two general-purpose SPI controllers; their allocation, including whether touch uses software-driven SPI, remains a feasibility decision. Espressif also identifies performance instability for concurrent WiFi-sniffer/BLE operation, so scheduled windows require measured coverage and switching limits. See [SPI controller documentation](https://docs.espressif.com/projects/esp-idf/en/release-v4.4/esp32/api-reference/peripherals/spi_master.html) and [RF coexistence documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/coexist.html).

Standalone observation requires continued power. Removing the sole USB power cable stops monitoring; the MVP does not claim uninterrupted operation across that removal.

## Published peripherals and pins

| Peripheral | Controller or function | ESP32 pins |
|---|---|---|
| Display | ILI9341V, 4-wire SPI | CS 15, D/C 2, SCLK 14, MOSI 13, MISO 12, reset shared with EN, backlight 21 |
| Resistive touch (E32R28T) | XPT2046, SPI | SCLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| RGB LED | Common anode, active low | red 22, green 16, blue 17 |
| MicroSD | SPI | CS 5, MOSI 23, SCLK 18, MISO 19 |
| Audio | Enable and DAC output | enable 4 (active low), DAC 26 |
| Buttons | Download and reset | download 0, reset EN |
| UART0 / USB serial | RX and TX | RX 3, TX 1 |
| Battery measurement | ADC input | 34 |
| External SPI CS | Shared MicroSD bus | CS 27 |
| Input-only expansion | GPIO input | 35 |

Treat these assignments as vendor claims until exercised by a profile smoke test. Several pins have ESP32 boot-strapping or input-only constraints; firmware must not repurpose them casually.

The board schematic labels the audio amplifier `SC8002B`. LCDWiki separately links an `FM8002E` datasheet; the parts have the same firmware-facing enable behavior and SOP-8 pinout but different electrical limits. See [`audio-amplifier-comparison.md`](audio-amplifier-comparison.md). The page also calls the external shared bus I²C in one place; its pin table and schematic show SPI, which this profile follows.

## Profile requirements

- Treat resistive touch as required on the active E32R28T unit.
- Run guided touch calibration on first start before accepting normal navigation input.
- Preserve compatible calibration across firmware updates; invalidate it when the hardware profile, landscape coordinate model, or calibration schema changes.
- Allow recalibration from normal on-device settings and through a USB recovery command that clears calibration and restarts the guide.
- Calibrate against five guided targets, validate against separate targets, reject excessive error, and checksum the stored transform together with its profile, orientation, and schema identity.
- Use a 320×240 landscape layout.
- Expose board supply/battery voltage only as a diagnostic value; do not claim calibrated state of charge, battery health, or portable runtime in the MVP.
- Treat the audio amplifier and speaker as an unverified, deferred capability tracked by [`FR01`](../../../docs/tickets/FR01.md). Keep the dormant driver disabled by default. Screen and RGB severity indication remain the complete MVP Alert cues.

## Files

- [`profile.toml`](profile.toml) is the machine-readable board description.
- [`audio-amplifier-comparison.md`](audio-amplifier-comparison.md) resolves the schematic/datasheet naming discrepancy.
- [`development/`](development/) is reserved for project-owned setup, build, flash, and debugging material.
- [`vendor/`](vendor/) contains a local ignored cache of unmodified public source material plus tracked provenance and checksums.

LCDWiki does not state a redistribution license on the product page or in the inspected archives. Vendor binaries and documents are ignored by Git; keep them unmodified and preserve their source URLs. Project-authored code must not assume the vendor examples' licensing terms.
