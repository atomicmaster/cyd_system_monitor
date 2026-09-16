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

### Controller-only BLE result

The F08 controller-only VHCI experiment is also rejected as a capacity path:
the real UI plus Wi-Fi and the bounded passive HCI scanner still overflowed
DRAM by 13,008 B and IRAM by 2,140 B. It was not flashed. The exact
configuration, command, and comparison with the NimBLE attempt are recorded
in [the controller-only investigation](ble-controller-vhci-investigation.md).

### Passive Wi-Fi monitor result

The next capacity experiment replaced station Wi-Fi with a `WIFI_MODE_NULL`,
management-frame-only promiscuous monitor. It uses the smallest documented
receive pools, retains AMPDU RX, removes connection/security/SoftAP features,
and disables the Wi-Fi IRAM speed optimizations. The combined controller-only
BLE image **still did not link**: DRAM overflow was **12,632 B**, a recovery
of only **376 B** from the controller-only result. IRAM did fit, eliminating
the former 2,140 B overflow. This is a meaningful IRAM recovery, but it is
not enough DRAM to flash or validate on the board, and it is not product
policy. The passive API path, exact overlay, resolved configuration, and
build command are recorded in [the passive Wi-Fi investigation](wifi-passive-monitor-investigation.md).

### 32 KiB LVGL-pool radio result

The physical F08a UI exercise recorded a 10,908 B LVGL peak across its
capacity slice, so the same radio probe was rebuilt with only the LVGL
allocation pool changed from 64 KiB to 32 KiB. It **linked** with 20,140 B
of static DRAM and 23,693 B of IRAM remaining. This confirms that the pool,
not Wi-Fi trimming, was the practical link-time DRAM lever.

The build was not flashed: its binary was 1,143,792 B (`0x1173f0`), while the
current 2 MiB-flash profile uses ESP-IDF's 1 MiB single-app factory partition.
It consequently failed the partition-size check by 95,216 B (`0x173f0`). A
partition-layout decision or a separate flash-size reduction is now required
before runtime radio/heap/capture measurement; a linker-successful ELF alone
is not sufficient evidence that the combined workload fits.

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

## F08a UI capacity slice: build-time result (no physical board)

[F08a](../../../../docs/tickets/F08a.md) adds the bounded operational UI
capacity slice (live status, a maximum-size Alert list, Alert detail, and
Settings navigation), the LVGL pool/heap/task-stack capacity probe
(`DEV:CAPACITY_STATUS`), and simulator coverage of the declared row/string
bounds under repeated navigation (`./dev run simulator -- capacity`). This
section is the build-time-only result recorded before a board became
available; see "F08a physical board result" below for the real E32R28T
session and why it still leaves Work item 5 incomplete.

Building the ordinary (non-probe) diagnostic image plus this ticket's
instrumentation and capacity-slice UI, unmodified from the profile's default
`sdkconfig.defaults`, with:

```sh
source "$HOME/esp/esp-idf/export.sh"
cd firmware
idf.py build
idf.py size
```

produced a linking image (no WiFi/BLE) with:

| Region | Used | Used % | Remaining |
| --- | ---: | ---: | ---: |
| DRAM | 109,656 bytes | 88.02% | 14,924 bytes |
| IRAM | 65,378 bytes | 49.88% | 65,694 bytes |
| Flash (.bin) | 623,392 bytes | — | 0x67c70 bytes (41%) free of the 1 MiB app partition |

This is UI/runtime-only headroom with no radio host linked at all, so it
cannot be read as combined-load capacity, and DRAM headroom (14,924 bytes)
is already well below the ~13,296+22,576 byte DRAM overflow the WiFi+NimBLE
and WiFi+Bluedroid build-time probes recorded above. A capacity change
(smaller UI/runtime footprint, a lower-level BLE receiver, or a
higher-memory profile) therefore remains necessary regardless of which
radio approach M1a eventually selects; the UI slice this ticket adds is not,
by itself, evidence that any radio approach will now fit. The declared
row/string bounds in `firmware/domain/include/firmware/domain/capacity.hpp`
and the `DEV:CAPACITY_STATUS` UART command exist so a future physical
session can produce the actual peak-usage and low-water-mark numbers this
entry is missing.

The diagnostic screen's new "Capacity slice" button (touch-present builds
only) reaches the bounded slice, pre-loaded with a maximum-size placeholder
Alert/Settings fixture (`firmware::ui::capacity::MakeMaxRepresentativeState`)
since no real Host Alert/Settings source exists yet; tapping the live
capacity-slice screen advances Live status -> Alert list -> Alert detail ->
Settings -> back to the diagnostic screen. `./dev run hardware -- capacity
--port <path>` is the intended session entrypoint once a board is
available.

## F08a physical board result

A connected E32R28T became available after the above was recorded. This
session flashed and exercised commit `88899f08e1e56aa2d34f6ea4492eec165823f202`
on port `/dev/cu.usbserial-140` at 115200 baud, using
`idf.py -p /dev/cu.usbserial-140 flash` followed by a pyserial script
(DTR/RTS reset, then line-based `DEV:` commands) rather than an interactive
`idf.py monitor` session, since the operator here is an agent without a
terminal to type into.

**Boot and diagnostic-screen baseline (real hardware):**

- Boots reliably from power-on reset, reaches `boot: showing diagnostic
  screen (touch_ready=1 have_valid_calibration=1)` -- a prior calibration
  record from an earlier F06 session was still valid in NVS.
- `DEV:PERIPHERAL_STATUS`: microsd mounted, battery 2129-2130mV, boot
  button released, expansion floating -- matches F07's prior evidence.
- `DEV:CAPACITY_STATUS`, sampled twice a few seconds apart while idle on
  the diagnostic screen (not the capacity slice -- see blocker below):

  | Reading | Value |
  | --- | ---: |
  | LVGL pool total | 63,424 bytes |
  | LVGL pool peak used | 10,348 bytes |
  | LVGL pool available | 53,704 bytes |
  | Free heap | 178,416 bytes |
  | Minimum free heap | 162,192 bytes |
  | `main` task stack low-water mark | 12,792 bytes |
  | `dev_console` task stack low-water mark | 1,328 bytes |

  Both samples were identical, confirming the probe is stable and
  `DEV:CAPACITY_STATUS` is safe to re-issue live. This is real measured
  evidence, but it is the **diagnostic-screen lower bound** F08a's Outcome
  explicitly distinguishes from "the MVP's realistic worst normal UI
  state" -- the capacity slice itself was never built during this session,
  so its own LVGL pool/heap peak is not yet known.

**First attempt: driven over UART only, no tap reached the slice.** The
session above was driven entirely over the UART/dev-console channel (a
pyserial script sending `DEV:` lines), with no human present at the board
to physically press the touchscreen -- the continuous `touch-diagnostic`
log correctly showed `irq_asserted=0` throughout, since nothing was
touching the panel (not a driver fault: F06's evidence already confirms
real taps assert IRQ correctly on this same touch driver). There is
deliberately no UART/dev-console path to open the capacity slice as a
substitute for a real tap, since Work item 5 asks for an *active-touch*
exercise.

**Second attempt: an operator present at the board, real taps registered,
but navigation never advanced past the Alert list/Settings screens.**
With the operator physically tapping, `indev press:` log lines confirmed
LVGL was receiving real touch coordinates and
`capacity_slice_button: LV_EVENT_CLICKED` fired correctly, entering the
capacity slice. But taps on the Alert list and Settings screens (both
built as a near-full-screen scrollable list, wired for
`LV_EVENT_CLICKED` on the screen *root*) never advanced: LVGL delivers
press/release to the list object (which consumes it for its own
scroll-gesture detection) rather than bubbling the click to root. This
was a real bug in the exercise wiring, not a touch-hardware limitation.

**Fix and third attempt: dedicated "Next" button, confirmed working.**
Added a real button (`CapacitySliceController::next_button()`) present on
every capacity-slice screen, outside the scrollable list, and wired
`main.cpp`'s advance handler to it instead of the root
(commit `cdfefb6019bfe8bb997378f920f96b842f53b253`, see its message for
the full fix). Reflashed and
rerun with the same operator: `capacity_slice_button: LV_EVENT_CLICKED`
entered the slice, and four paced taps on "Next" cycled Live status ->
Alert list -> Alert detail -> Settings, each confirmed by a distinct
`DEV:CAPACITY_STATUS` reading polled every ~4s during the sequence:

| Screen | LVGL pool total | LVGL pool available |
| --- | ---: | ---: |
| Diagnostic (baseline) | 63,424 bytes | 53,704 bytes |
| Live status | 63,728 bytes | 56,760 bytes |
| Alert list (8 rows, clamped) | 63,536 bytes | 54,968 bytes |
| Alert detail | 63,704 bytes | 56,504 bytes |
| Settings (8 rows, clamped) | 63,536 bytes | 55,016 bytes |

LVGL pool peak used stayed flat at 10,908 bytes across every reading in
this window (the pool's own running high-water mark had already been set
by earlier screen builds and was never exceeded again), free heap stayed
at 176,424 bytes throughout the capacity slice (178,416 bytes on the
diagnostic screen) with minimum free heap unchanged at 167,808 bytes, and
`main`/`dev_console` task stack low-water marks stayed flat at
12,744/1,952 bytes. No crash, no watchdog trip, no visible slowdown; the
board remained fully responsive to touch throughout, and normal reset
afterward returned it to the diagnostic screen cleanly (board recoverable).

**Consequence for F08a's status:** Work items 1-6 are now confirmed live
on real hardware: the probe, its declared bounds, and the capacity
slice's own tap-driven navigation and per-screen LVGL pool/heap readings
all match the simulator's coverage and the code's declared behavior.
Remaining gaps are measurement breadth, not a missing capability: this
session did not exercise the slice under concurrent one-second serial
traffic or flash-write activity (F08's combined-load scope), and used
placeholder Alert/Settings content (`MakeMaxRepresentativeState`) rather
than real Host data, since no real Alert/Settings source exists yet.
