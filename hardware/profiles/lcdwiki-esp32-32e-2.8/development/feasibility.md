<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# M1a combined hardware feasibility record

**Status: blocked — do not advance to M2.**

This record exists to make the M1a capacity result explicit rather than
claiming that scheduled WiFi and BLE observation fit before the physical run.
It is not physical-board evidence.

## Build-time capacity result

On 2026-09-16, the probe started from commit
`6e43931f01f5d60c936d2d16c58c29d465625986` using ESP-IDF v5.3.2 and the
active `lcdwiki-esp32-32e-2.8` profile (no PSRAM). It retained the LVGL
diagnostic image, initialized passive WiFi support and BLE-only NimBLE
scanning, then ran:

```sh
source "$HOME/esp/esp-idf/export.sh"
cd firmware
idf.py reconfigure
idf.py build
```

The exact configuration overlay is retained in
[`m1a-capacity-probe-sdkconfig.defaults`](m1a-capacity-probe-sdkconfig.defaults)
and the minimal source overlay that forces both hosts to link is retained in
[`m1a-capacity-probe.patch`](m1a-capacity-probe.patch). Apply both at the
recorded commit, then reconfigure and build with the command above. The
partition setting was `CONFIG_PARTITION_TABLE_SINGLE_APP=y` with the
profile's 1 MiB application partition. Peripheral, broadcaster, security,
and NimBLE NVS persistence roles were disabled.

The probe is an intentionally reversible build-time capacity observation, not
a release image. The retained overlays let the F05-F07 diagnostic image stay
buildable while preserving the failure for independent reproduction. A
selected replacement radio approach must be committed and rebuilt before it
can provide physical closure.

The build's linker output was:

```text
region `dram0_0_seg' overflowed by 13296 bytes
region `iram0_0_seg' overflowed by 2008 bytes
```

The linker therefore rejected the image:

| Region | Overflow |
| --- | ---: |
| Internal DRAM | 13,296 bytes |
| IRAM | 2,008 bytes |

The default Bluedroid attempt was larger, overflowing DRAM by 22,576 bytes.
The attempted configurations are deliberately not retained as firmware
defaults, so the established F05–F07 diagnostic image remains buildable.

## Consequence

The current UI configuration and full ESP-IDF BLE host cannot be accepted as
the M1a combined-load baseline for this no-PSRAM E32R28T. Before a physical
run can close the gate, the project must choose and validate a capacity
change, such as a materially smaller UI/runtime footprint, a lower-level BLE
receiver that does not pull in a full host, or a supported profile with more
memory. That decision must preserve the product requirement for passive BLE
observation and cannot silently defer it to M5.

## Accounting rules already fixed

The portable feasibility assessment tests enforce the two claims that later
measurements must retain:

- UART capacity is calculated with 8N1 framing and a one-second,
  1,024-byte representative snapshot needs 10,240 bits/s; at 115,200 baud
  this leaves 104,960 bits/s before protocol overhead is refined by C01.
- Requested Observation Window time is never verified receive time. Only a
  measured receiver-available interval counts; all other requested time is a
  Coverage Gap, with uncertainty retained as a separate measurement field.

The resource-budget fixture uses bounded active/ended Alert and Transmitter
Baseline record sizes plus settings, reclamation, Operational Alert, and
Capacity-pressure Alert reservations. Its counts are test inputs, not a
promise that 32/128/512 fit the final partition.

## Required next physical evidence

After selecting a buildable radio approach, reserve the board and record the
flashed commit, port, exact config, measurement method, radio load, display
and touch behavior, NVS write rate, one-second serial traffic, minimum free
RAM, firmware image size, channel revisit timing, and packet loss. Compare
the current software-touch/SPI2-display/SPI3-MicroSD arrangement with an
alternative under that same load. This document remains blocked until those
measurements and a provisional partition/write/endurance budget are present.
