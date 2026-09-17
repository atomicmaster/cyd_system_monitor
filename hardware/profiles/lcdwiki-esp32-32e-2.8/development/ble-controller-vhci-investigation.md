<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# F08 investigation: controller-only passive BLE reception

**Status: rejected for the current E32R28T UI/Wi-Fi baseline — a reproducible
controller-only build still overflows internal DRAM and IRAM.**

> **Superseded (2026-09-17).** The DRAM overflows that motivated this
> investigation were caused by two static buffers competing for the ESP32's
> single linkable DRAM segment, not by a shortage of memory. With the LVGL
> pool and display draw buffer moved to the runtime heap, the full NimBLE
> host links alongside Wi-Fi and the complete UI with roughly 77 KB of static
> DRAM to spare. The controller-only path below still works and remains a
> legitimate design option, but it is no longer required, and the capacity
> conclusion in the status line above is wrong. See
> [the feasibility record](feasibility.md#capacity-blocker-resolved-two-measurement-errors).

This note investigates the narrow alternative to the failed Wi-Fi + NimBLE
probe: keep Espressif's BLE controller, but replace the NimBLE host with a
small, application-owned HCI command/event loop. It applies to the original
ESP32 used by the `lcdwiki-esp32-32e-2.8` profile and ESP-IDF v5.3.2.

## Supported path

ESP-IDF explicitly offers `CONFIG_BT_CONTROLLER_ONLY` for communicating
directly with the controller without a host, or with a non-Espressif host.
The original-ESP32 controller Kconfig offers VHCI and calls it the normal
choice when the host also runs on the ESP32. The shipped ESP32-targeted
`ble_adv_scan_combined` example enables BLE-only controller mode, disables
Bluedroid, selects `CONFIG_BT_CONTROLLER_ONLY=y`, initializes and enables
the controller, then registers a VHCI callback. Its README explicitly says
that no host is used and that necessary host functions are implemented by the
application. These are direct
evidence that controller-only VHCI is a supported configuration on this
target, rather than an undocumented controller binary interface.

Sources:

- [ESP-IDF v5.3.2 Bluetooth Kconfig, host choice](https://github.com/espressif/esp-idf/blob/v5.3.2/components/bt/Kconfig#L7-L36)
- [ESP-IDF v5.3.2 original-ESP32 controller VHCI Kconfig](https://github.com/espressif/esp-idf/blob/v5.3.2/components/bt/controller/esp32/Kconfig.in#L172-L184)
- [ESP-IDF v5.3.2 ESP32 controller-only scan example README](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/ble_adv_scan_combined/README.md)
- [ESP-IDF v5.3.2 ESP32 controller-only scan configuration](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/ble_adv_scan_combined/sdkconfig.defaults)
- [ESP-IDF v5.3.2 ESP32 controller-only scan initialization](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/ble_adv_scan_combined/main/app_bt.c#L375-L445)

`esp_vhci_host_register_callback` supplies both controller-to-application
packets and a notification that the controller is ready for the next host
packet. The documented send API requires checking readiness before every
send; it must not be used in a critical section or while the scheduler is
suspended. Packets received through the callback therefore need a bounded,
non-blocking handoff to an application parser rather than storage or UI work
inside the callback.

Sources:

- [ESP-IDF v5.3.2 VHCI API declarations and send restrictions](https://github.com/espressif/esp-idf/blob/v5.3.2/components/bt/include/esp32/include/esp_bt.h#L497-L535)
- [ESP-IDF v5.3.2 scan example VHCI callbacks](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/ble_adv_scan_combined/main/app_bt.c#L38-L92)

## What the application would own

There is no GAP/scan API above VHCI in controller-only mode. The application
must act as the minimal HCI host:

1. Initialize NVS (for PHY calibration), release Classic-Bluetooth memory,
   initialize/enable the BLE controller, and register VHCI callbacks, as the
   official example does.
2. Serialize HCI commands only when the send-available API permits them;
   observe Command Complete/Command Status before advancing the setup state.
3. Send H4 HCI Reset, set the required event masks, issue LE Set Scan
   Parameters with **passive** scan type, then LE Set Scan Enable. The
   official HCI helper component includes wire encoders for the two scan
   commands and identifies H4 event packets, LE Meta Events, and LE
   Advertising Reports.
4. Parse only the desired LE Advertising Report payloads, validate lengths,
   filter to the product's transmitter identifiers, copy no more than the
   declared bounded observation record, and release/drop all other packets.
   It must also count malformed, dropped, and duplicate reports and record
   receiver-available time, because controller-only HCI does not supply
   NimBLE's GAP callbacks, duplicate policy, or lifecycle recovery.
5. Explicitly disable scanning for planned Wi-Fi/UI-sensitive intervals and
   re-enable it through the same state machine. A controller reset, VHCI
   command failure, or queue overflow is an Observation Gap, not proof that
   the requested window was observed.

The command names and event identifiers in item 3 are from Espressif's own
example component; their presence establishes the packet-level interface,
not a promise that every advertising transmitter will be received.

Sources:

- [ESP-IDF v5.3.2 HCI command encoders: Reset and scan commands](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/hci_common_component/bt_hci_common.c#L15-L44)
- [ESP-IDF v5.3.2 H4/LE event and scan-command definitions](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/hci_common_component/include/bt_hci_common.h#L14-L59)
- [ESP-IDF v5.3.2 controller-only scan lifecycle](https://github.com/espressif/esp-idf/blob/v5.3.2/examples/bluetooth/hci/ble_adv_scan_combined/main/app_bt.c#L375-L445)

## Coexistence is still a real constraint

Controller-only removes the **host**, not the BLE controller, its radio use,
or ESP32 Wi-Fi/Bluetooth coexistence behavior. Espressif says ESP32 BLE scan
may be interrupted by Wi-Fi; it specifically documents
`CONFIG_BTDM_CTRL_FULL_SCAN_SUPPORTED` to let BLE reacquire RF resources
within the current scan window after Wi-Fi releases it. The option is
available in BLE-only mode and defaults enabled; its Kconfig describes it as
for high scan-performance cases. These facts make a scheduled scan/revisit
measurement mandatory, even if the controller-only image fits.

For ESP32 coexistence, Espressif recommends placing Wi-Fi protocol tasks and
Bluetooth controller/host tasks on different CPUs, and says software
coexistence must be enabled for the documented coexistence behavior. With no
Bluetooth host, retain the controller/core placement part of that guidance;
the application HCI parser should run as a small bounded task and must be
measured rather than assumed to have NimBLE's task behavior.

Sources:

- [ESP-IDF v5.3.2 coexistence guide: ESP32 scan interruption and full scan](https://github.com/espressif/esp-idf/blob/v5.3.2/docs/en/api-guides/coexist.rst#L219-L230)
- [ESP-IDF v5.3.2 ESP32 full-scan Kconfig](https://github.com/espressif/esp-idf/blob/v5.3.2/components/bt/controller/esp32/Kconfig.in#L393-L402)
- [ESP-IDF v5.3.2 coexistence guide: configuration and core placement](https://github.com/espressif/esp-idf/blob/v5.3.2/docs/en/api-guides/coexist.rst#L220-L227)

## Memory trade-off and decision

This approach should remove the linked NimBLE host, its transport-buffer
pool, and host task from the image; it **cannot** remove the BLE controller
allocation because the controller is still initialized and scanning. That is
a material architectural reduction, but not a quantified saving until the
same profile is linked and sized. The known M1a Wi-Fi + minimized-NimBLE
probe overflowed DRAM by 13,296 bytes and IRAM by 2,008 bytes; therefore a
successful link alone is insufficient. It must leave measured runtime margin
after the F08a UI slice and Wi-Fi are active.

The existing ESP-IDF documentation supports the qualitative distinction:
NimBLE is an ESP-IDF-supported **host** above the same underlying controller
and uses a transport layer with a controller/host buffer pool. Controller-only
selects no Espressif host. Do not claim a byte saving based only on this
structure.

Sources:

- [M1a measured overflow record](feasibility.md#build-time-capacity-result)
- [ESP-IDF v5.3.2 NimBLE architecture and transport-buffer responsibility](https://github.com/espressif/esp-idf/blob/v5.3.2/docs/en/api-reference/bluetooth/nimble/index.rst#L7-L18)
- [ESP-IDF v5.3.2 controller-only host choice](https://github.com/espressif/esp-idf/blob/v5.3.2/components/bt/Kconfig#L7-L36)

## Recommended experiment

Implement a reversible F08 probe, not product radio policy, with these
limits:

- `CONFIG_BT_ENABLED=y`, `CONFIG_BT_CONTROLLER_ONLY=y`, BLE-only controller
  mode, VHCI, Classic memory released, and no NimBLE/Bluedroid;
- a fixed HCI command state machine: Reset -> masks -> passive scan parameters
  -> scan enabled;
- one bounded HCI-event queue and a fixed-size parser/observation record;
  no advertisement retention, dynamic allocation, connection, active scan,
  GATT, pairing, or advertising;
- controller and Wi-Fi pinned to the separate ESP32 cores recommended by the
  coexistence guide, with software coexistence and full scan enabled for the
  comparison; and
- the same diagnostic/capacity-slice UI, Wi-Fi initialization, partition
  layout, and `idf.py size` procedure as the failed M1a probe.

First compare the controller-only image's DRAM/IRAM/flash size with the
recorded NimBLE probe. Only if it links with a deliberate runtime margin,
flash it and run F08's physical schedule while recording HCI queue drops,
command failures, scan enable/disable timestamps, Wi-Fi intervals, actual
Advertising Reports, and UI/touch/serial/flash behavior. This test can prove
or reject the lower-level path without prematurely making it the product
implementation.

## Experiment result (2026-09-16)

The reversible probe was implemented in
`firmware/platform/esp32/radio/controller_probe.cpp`. It starts the real
capacity-slice/diagnostic UI, initializes Wi-Fi in STA mode on one core, and
initializes the BLE-only controller on the other core. Its bounded four-event
static HCI queue separates the VHCI callback from a parser that issues Reset,
the HCI/LE event masks, passive scan parameters (50 ms interval, 30 ms
window), and Scan Enable. It retains no advertising payload; queue drops,
malformed events, command failures, and Advertising Report counts would be
reported through `DEV:HCI_STATUS` if the image linked.

The isolated build used the tracked
`firmware/sdkconfig.controller_only_probe.defaults` overlay, so the normal
diagnostic `sdkconfig` and image were not changed:

```sh
source "$HOME/esp/esp-idf/export.sh"
cd firmware
idf.py -B build-controller-only \
  -D SDKCONFIG=build-controller-only/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.controller_only_probe.defaults' \
  build
```

The ESP-IDF v5.3.2 linker rejected the resulting image:

| Probe | DRAM overflow | IRAM overflow |
| --- | ---: | ---: |
| Wi-Fi + minimized NimBLE host | 13,296 B | 2,008 B |
| Wi-Fi + controller-only VHCI, four-event parser queue | 13,008 B | 2,140 B |

The controller-only path recovers only 288 B of DRAM and adds 132 B of IRAM
pressure relative to the prior NimBLE experiment. Reducing the bounded queue
further cannot plausibly recover the required 13 KB-plus DRAM deficit and
would invalidate the parser's burst-handling evidence. The image was not
flashed: a physical radio/UI test cannot be performed without a linkable
image. This rejects controller-only VHCI as the near-term capacity change for
this board/profile, not VHCI as a supported ESP-IDF interface.
