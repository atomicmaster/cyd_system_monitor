<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# M1a combined hardware feasibility record

**Status: the capacity blocker is resolved; M1a's remaining physical
measurements are still outstanding. Do not advance to M2 yet.**

This record exists to make the M1a capacity result explicit rather than
claiming that scheduled WiFi and BLE observation fit before the physical run.

Sections below are in the order they were recorded. Everything from
"Build-time capacity result" through "First live controller-only radio
result" describes a memory shortage that **did not exist**: see
[Capacity blocker resolved](#capacity-blocker-resolved-two-measurement-errors)
for the two measurement errors those sections rest on, and treat their
conclusions as superseded.

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

### Large single-app partition result

The 2 MiB target flash was then rebuilt with ESP-IDF's documented 1.5 MiB
single-app, no-OTA partition layout. The same 32 KiB-LVGL, passive-Wi-Fi, and
controller-only-BLE image completed successfully: its 1,143,792 B binary has
389,136 B (26%) free in the 1,536,000 B app partition. This is still a
reversible probe overlay, not a production partition decision. It establishes
that the image is now eligible for a physical radio/runtime experiment.

### First live controller-only radio result

On 2026-09-17, commit `6344ddc` was flashed to the known E32R28T on
`/dev/cu.usbserial-140` with the 32 KiB LVGL, passive-Wi-Fi,
controller-only-BLE, and large-single-app probe overlays. The boot log
confirmed the 2 MiB flash and the 1.5 MiB factory partition. It reached the
diagnostic screen, mounted MicroSD, initialized Wi-Fi in null/sniffer mode,
and enabled the BLE controller.

Settled UART samples at roughly 27 s and 39 s uptime showed BLE advertising
reports increasing 1,033 -> 1,479 and Wi-Fi management frames 136 -> 197.
At roughly 96 s, the counters were 3,754 and 450. `command_failures`,
`malformed_events`, and `dropped_events` remained zero. The stable resource
readings were 114,416 B free heap (98,124 B minimum), 10,348 B LVGL-pool
peak with 21,068 B available from a 30,788 B usable pool, and 12,560/1,340 B
low-water stack headroom for `main`/`dev_console`.

This establishes that the image boots and receives both frame classes without
an observed early queue failure. It is not a packet-loss, coexistence-window,
channel-revisit, continuous-load, or active-touch UI acceptance result.

## Capacity blocker resolved: two measurement errors

On 2026-09-17 the M1a capacity blocker was traced to two errors in how the
probes above measured the board, not to a genuine shortage of memory. After
correcting both, the full UI configuration links and runs alongside Wi-Fi
and BLE with substantial headroom, at the original 64 KiB LVGL pool. No
UI/runtime footprint reduction, no lower-level BLE receiver, and no
higher-memory profile is required.

### Error 1: static DRAM segment mistaken for total DRAM

On ESP32 the linker can place static `.data`/`.bss` into only one DRAM
segment. The runtime heap additionally spans DRAM regions the linker cannot
place static data into at all: this board's boot log reports usable heap
across `3FFAFF10`, `3FFB6388`, `3FFB9A20`, `3FFC8F58`, `3FFE0440`, and
`3FFE4350`, of which only part is linkable.

Two of this firmware's buffers were declared as static arrays and so
competed for that one linkable segment:

| Static allocation | Size |
| --- | ---: |
| LVGL allocation pool (`LV_MEM_SIZE`, `lv_conf.h`) | 65,536 bytes |
| LVGL 40-row partial draw buffer (`display.cpp`) | 25,600 bytes |
| Total | 91,136 bytes |

Together these consumed roughly three quarters of the linkable segment.
Every "DRAM overflowed by N bytes" result above is that segment filling up
while the heap still had well over 100 KB free. The 32 KiB-LVGL-pool
experiment appeared to work for exactly this reason, and was read at the
time as evidence that the UI needed to be smaller. It is not: halving the
pool simply moved 32 KiB out of the contended segment.

The fix moves both buffers to the runtime heap, where they were always
affordable, and leaves their sizes unchanged:

- `firmware/ui/lv_conf.h` sets `LV_MEM_ADR 0` with `LV_MEM_POOL_ALLOC
  malloc`, so LVGL obtains its 64 KiB pool from the C heap at `lv_init()`.
  `lv_mem_monitor()` still reports pool peak/available, so
  `DEV:CAPACITY_STATUS` is unaffected.
- `firmware/platform/esp32/display/display.cpp` obtains the draw buffer via
  `heap_caps_malloc(..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`, preserving
  the DMA-capable internal-RAM requirement, and fails the display init
  explicitly if the allocation does not succeed.

Total RAM consumption is unchanged. Only placement changed, which is why
free heap in the live run below is correspondingly lower than the
pre-radio F08a readings.

### Error 2: 2 MiB flash assumed on a 4 MiB part

The probes above inherited ESP-IDF's default `CONFIG_ESPTOOLPY_FLASHSIZE`
of 2 MB and its 1 MiB single-app partition, and read the resulting
partition-size failure as a hardware limit. The board carries a 4 MB part:
`profile.toml` declares `flash_mb = 4`, `esptool.py flash_id` reports
"Detected flash size: 4MB", and the boot log of the live run below reports
`SPI Flash Size : 4MB`. The "Large single-app partition result" section's
1.5 MiB layout was therefore a workaround for a constraint that does not
exist.

`firmware/sdkconfig.defaults` now selects the 4 MB part and the project's
own `firmware/partitions.csv` (ADR 0010: one application slot, persistent
state in dedicated internal-flash partitions), giving a 2 MiB `factory`
app partition and a 1.94 MiB `state` partition.

### Corrected build-time results

All three built at the unchanged 64 KiB LVGL pool with ESP-IDF v5.3.2 and
the `lcdwiki-esp32-32e-2.8` profile. "Remaining" is linkable static space
reported by `idf.py size`; the DRAM total differs between rows because the
radio configurations reserve controller and Wi-Fi memory.

| Configuration | Static DRAM remaining | IRAM remaining | Image |
| --- | ---: | ---: | ---: |
| No radio (ordinary image) | 167,076 B (92.4%) | 67,866 B (51.8%) | 612,816 B |
| Controller-only BLE + passive Wi-Fi | 78,508 B (63.0%) | 22,666 B (17.3%) | 1,143,792 B |
| Full NimBLE host + Wi-Fi | 77,304 B (62.1%) | 23,690 B (18.1%) | 1,192,772 B |

Both radio images fit the 2 MiB `factory` partition with 45% and 43% free
respectively. The previously recorded DRAM overflows of 13,296 B (NimBLE),
22,576 B (Bluedroid), 13,008 B (controller-only), and 12,632 B
(controller-only with passive Wi-Fi) are all gone.

The full NimBLE row is the significant one: **the M1a product requirement
for passive BLE observation no longer needs the low-level controller-only
VHCI path.** Reproduce it by building `sdkconfig.defaults` together with
[`m1a-capacity-probe-sdkconfig.defaults`](m1a-capacity-probe-sdkconfig.defaults)
and [`m1a-capacity-probe.patch`](m1a-capacity-probe.patch)'s force-link of
`esp_wifi_init()` and `nimble_port_init()`, plus
`CONFIG_ESP_WIFI_IRAM_OPT=n` and `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`. Without
those two Wi-Fi IRAM options the NimBLE image overflows IRAM by 1,008 bytes
and nothing else; DRAM fits either way. Which BLE host M1a finally adopts
is now a design choice, not a capacity forced move, and that choice is not
made here.

### Live board result

Commit `6aa298f` plus the working-tree fixes above was flashed to the known
E32R28T on `/dev/cu.usbserial-140` in the controller-only-BLE plus
passive-Wi-Fi configuration, built as:

```sh
source "$HOME/esp/esp-idf/export.sh"
cd firmware
idf.py -B build-radio-heap -D SDKCONFIG=build-radio-heap/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.controller_only_probe.defaults;sdkconfig.passive_wifi_probe.defaults' \
  build
idf.py -B build-radio-heap -p /dev/cu.usbserial-140 flash
```

The boot log confirmed `SPI Flash Size : 4MB` and loaded the project
partition table (`factory` 0x200000 at 0x10000, `state` 0x1f0000 at
0x210000). The board reached the diagnostic screen, mounted MicroSD,
brought up Wi-Fi in null/sniffer mode, and enabled the BLE controller.

Resource readings were identical at 12 s, 42 s, 92 s, and 112 s of uptime:

| Reading | Value |
| --- | ---: |
| LVGL pool total | 63,424 bytes |
| LVGL pool peak used | 10,348 bytes |
| LVGL pool available | 53,704 bytes |
| Free heap | 81,516 bytes |
| Minimum free heap | 65,232 bytes |
| `main` stack low-water mark | 12,796 bytes |

The heap-allocated 64 KiB pool therefore behaves exactly as the static one
did, and the heap-allocated draw buffer renders correctly. Free heap is
flat across the run, so neither allocation leaks.

Radio counters over the same window:

| Uptime | BLE advertising reports | Wi-Fi management frames |
| ---: | ---: | ---: |
| 14 s | 436 | 96 |
| 44 s | 1,511 | 277 |
| 110 s | 3,896 | 713 |

`command_failures`, `malformed_events`, and `dropped_events` stayed at zero
throughout. MicroSD remained mounted and battery read 2,115-2,125 mV.

This is a boot, link, and sustained-reception result at 65,232 bytes of
minimum free heap. It is still **not** a packet-loss, coexistence-window,
channel-revisit, continuous-load, NVS-write, or active-touch-under-radio
acceptance result. Those remain required below.

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

A buildable radio approach now exists, and more than one: both the
controller-only VHCI path and the full NimBLE host link with headroom. The
capacity precondition is met, so what remains is measurement, not a
capacity decision.

Still to record on the board: one-second serial traffic and packet loss.
Touch behavior under combined radio load and WiFi channel-hop switching
gaps/revisit timing are now recorded below, though the channel-hop result
has not yet been combined with the touch/UI load session, or run under a
wider channel plan or shorter dwell. Compare the current
software-touch/SPI2-display/SPI3-MicroSD arrangement with an alternative
under that same load. A provisional partition/write/endurance budget
beyond the NVS write-rate evidence below is also still absent;
`firmware/partitions.csv` states its own sizes are a provisional M1a
budget rather than an endurance budget. M1a does not close
until those are present.

## CPU load and NVS write rate under combined load

On 2026-09-17, `DEV:CPU_STATUS` (FreeRTOS per-task runtime counters,
`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`) and `DEV:NVS_WRITE_TEST` were
added and exercised live on the same E32R28T, controller-only-BLE plus
passive-Wi-Fi build as the live radio result above (commit tree includes
this record's own changes; not yet a separate integrated commit).

**CPU load**, sampled over a 1 s window roughly 20 s after boot, with the
diagnostic screen idle (no active touch) and radio reception ongoing:

| Task | Core | Share of that core's time |
| --- | --- | ---: |
| `main` (LVGL timer handler, touch polling, dev console reads run on it) | 0 | 12.6% |
| `btController` | 0 | 1.1% |
| `hci_parser` | 1 | 0.08% |
| `wifi` | 1 | 0.07% |
| `esp_timer` | 0 | 0.04% |
| everything else (dev_console, tiT, sys_evt, ipc0/1, Tmr Svc) | both | <0.3% combined |
| idle (core 0 / core 1) | 0 / 1 | 85.9% / 99.9% |

Core 1 is almost entirely idle; core 0's ~14% busy is essentially all
`main`, which is the touch-diagnostic polling/LVGL timer loop rather than
the radio probe. This is well within the <1% *Host* CPU budget's spirit
for a Monitor Device with no equivalent stated firmware budget yet, but it
is an idle-screen reading: it does not include active touch or the
capacity slice's own screen churn, and `main` folding LVGL, touch, and
console I/O into one task (no dedicated UI task) means none of these can
be attributed separately without further instrumentation.

**NVS write rate**, 20 consecutive re-saves of the board's own real,
already-valid calibration record (unchanged, so the stored checksum stays
correct) while radio reception continued in the background:

| Metric | Value |
| --- | ---: |
| Writes attempted / failed | 20 / 0 |
| Min write latency | 762 μs |
| Max write latency | 915 μs |
| Average write latency | 775 μs |

`DEV:HCI_STATUS` immediately before and after this burst showed
`command_failures`, `malformed_events`, and `dropped_events` all remaining
zero (advertising reports and Wi-Fi management frames continued
incrementing throughout), so 20 back-to-back NVS blob writes did not
visibly disrupt radio reception on this build. Twenty writes is not an
endurance test -- NVS wear-leveling and flash sector erase-cycle limits are
unmeasured here -- but it establishes a real per-write latency figure
(roughly 0.8 ms) to size a settings/journal write budget against, rather
than the "provisional... not yet measured" gap the ticket previously left
open.

## Touch under combined radio load: live board result

On 2026-09-17, with a human operator physically at the same E32R28T
(controller-only-BLE plus passive-Wi-Fi build, radio reception continuous
throughout, `DEV:CPU_STATUS`/`DEV:HCI_STATUS` polled every 15 s over a
serial capture), the operator repeatedly entered and stepped through the
F08a capacity slice (Live status -> Alert list -> Alert detail -> Settings
-> back to diagnostic, several full loops), then tapped "Recalibrate,"
deliberately failed the five-point calibration once (to exercise the
failure/retry path), and completed a full five-point calibration
afterward, then returned to more capacity-slice loops. Total session
length was just over four minutes of active operator interaction.

**Detection latency.** The one directly comparable pair in this session --
`indev press` (raw touch-down detected) and `recalibrate_button:
LV_EVENT_PRESSED` (LVGL's own press-dispatch event) -- carried the same
logged timestamp (both at uptime 56,950 ms), i.e. touch-down detection to
LVGL event dispatch took less than the ~10 ms log-timestamp resolution.
This is the number the 100 ms processed-touch-input budget in
[mvp-validation.md](../../../../docs/mvp-validation.md) is actually about,
and it held with BLE scanning and Wi-Fi monitoring both running.

**Indev-press-to-click interval.** `indev press` to the resulting
`LV_EVENT_CLICKED` (fired on release) measured 30-130 ms across eight taps
on `capacity_slice_button` and `recalibrate_button`
(100, 90, 90, 100, 70, 30, 130, 100 ms). This interval is dominated by how
long the operator's finger stayed on the glass, not firmware queueing --
the 0 ms PRESSED-dispatch figure above already accounts for the
detection-side latency -- so it is reported as an upper bound on
system-attributable delay, not a floor.

**CPU load under active touch.** `DEV:CPU_STATUS` samples taken during the
five-point calibration's rapid touch sequences showed `main` (LVGL timer
handler, touch polling, dev console) at 30.2-33.9% of core 0, roughly
double the ~12-17% seen on the same core while idle on a menu screen
between taps (see the CPU-load table above). Core 1, carrying the BLE/Wi-Fi
tasks, stayed under 0.1% busy throughout, including during active touch.

**No degradation, no crash, board recoverable.** Across the full session,
`command_failures`, `malformed_events`, and `dropped_events` stayed at zero
while `advertising_reports` rose from 150 to 9,499 and
`wifi_management_frames` from 0 to 81. A single `boot: showing diagnostic
screen` line at the start of the log confirms no reset or crash occurred
despite the deliberate calibration failure and retry. A `DEV:CAPACITY_STATUS`
query afterward showed 76,304 B free heap (unchanged from the earlier
idle reading) and 72,892 B minimum free heap, so nothing measurable leaked
across the exercise; LVGL pool peak used rose from 10,348 B to 11,412 B
(the capacity slice's own screens, not radio activity, are the reason --
see the F08a physical result below for the per-screen breakdown) and
`main`'s stack low-water mark stayed at 12,540 B, both far from their
respective ceilings. `DEV:PERIPHERAL_STATUS` confirmed MicroSD stayed
mounted and the board fully responsive afterward.

This closes F08's touch-under-radio-load gap: active touch, including a
full calibration flow, coexists with continuous BLE/Wi-Fi reception with
no observed loss, no crash, and CPU/RAM headroom well inside budget. It
does not measure display flush time or a frame-accurate render latency --
only the detection-to-dispatch and detection-to-click intervals available
from existing log timestamps.

## WiFi channel-hop switching gaps and revisit timing

On 2026-09-17, `controller_probe.cpp` gained a bounded `ChannelHopTask`
that cycles a fixed 3-channel probe set (1, 6, 11 -- the standard
non-overlapping 2.4 GHz trio, not ADR 0026's eventual per-region Channel
Plan, which does not exist yet), timing each `esp_wifi_set_channel()` call
and logging it alongside the running BLE/WiFi counters. `DEV:CHANNEL_STATUS`
reports the cumulative switch count and last/max/average switch duration.
This measures whether explicit WiFi channel switching is affordable and
whether it visibly interrupts BLE reception on this single-radio target
(ADR 0003) -- it is not a scheduling policy, and it does not yet interleave
with an explicit BLE Observation Window on the same clock, since no such
scheduler exists yet.

An initial run used an arbitrary 1 s dwell (matching the Host snapshot
cadence, not beacon timing) and is recorded below for provenance, but the
dwell was then revised to align with 802.11 beacon timing, which is the
actually relevant clock for "is this channel switch fast enough to keep
observing WiFi Activity" -- see "Beacon-aligned dwell revision" below for
the current, superseding measurement.

### Initial run: 1 s dwell (superseded)

Run unattended (no operator needed) on the same controller-only-BLE plus
passive-Wi-Fi build, over 45 s and 38 channel switches at a 1 s dwell:

| Metric | Value |
| --- | ---: |
| Channel switches | 38 |
| Min / max switch duration | 557 / 709 μs |
| Average switch duration | 582 μs |
| Revisit period (3 channels x 1 s dwell) | 3.0 s |

`DEV:HCI_STATUS` showed `command_failures`, `malformed_events`, and
`dropped_events` all remaining zero throughout. More importantly, the
per-switch trace logged `advertising_reports` at every switch instant, and
the increment between consecutive switches never dropped near zero: over
the 38 switches the per-dwell-window increments ranged from 18 to 27 BLE
advertising reports, with no window showing a stall correlated with the
WiFi channel change. `wifi_management_frames` also climbed on every
channel (unevenly -- channel 1 accumulated more than 6 or 11 in this
office RF environment, which reflects real ambient channel occupancy, not
a probe artifact). `DEV:CPU_STATUS` during the run showed the `wifi` task
at under 0.1% of core 1 and the new `channel_hop` task's own overhead not
separately visible above that noise floor.

This 1 s figure was arbitrary, chosen only because it matched the Host
snapshot cadence elsewhere in the project; it was not checked against
whether it actually captured beacons, the frames the WiFi screen's
"sampled-channel Observed WiFi Activity" (per
[radio-model.md](../../../../docs/radio-model.md)) depends on for
identifying an Observed BSS.

### Beacon-aligned dwell revision

The 802.11 default beacon period (`dot11BeaconPeriod`) is 100 TU =
102,400 μs, and the overwhelming majority of consumer APs use exactly that
default. A dwell shorter than one beacon period can miss every beacon from
an AP on that channel purely from phase alignment, independent of RF
conditions -- that would be a probe artifact reported as a coverage gap,
not a real one. `kChannelDwellMs` was revised from the arbitrary 1 s to
300 ms: roughly three beacon periods, giving margin for this task's own
`vTaskDelay()` being quantized to `CONFIG_FREERTOS_HZ`'s 10 ms tick, for
APs that configure a longer-than-default beacon period, and for needing
more than one sighting to trust a miss. A new `beacon_frames` counter
(802.11 frame-control type 0 / subtype 8, read directly out of each
captured management frame, as distinct from probe/assoc/deauth/etc.
traffic already counted in `wifi_management_frames`) exists specifically
to check that margin against reality rather than assume it.

Run unattended over 46 s and 156 channel switches at the revised 300 ms
dwell:

| Metric | Value |
| --- | ---: |
| Channel switches | 156 |
| Min / max switch duration | ~547 / 761 μs |
| Average switch duration | 665 μs |
| Revisit period (3 channels x 300 ms dwell) | 0.9 s |
| Beacon frames observed (of 82 total management frames) | 50 |

Switch cost is unchanged from the 1 s-dwell run (paragraph above), as
expected -- the per-call cost of `esp_wifi_set_channel()` does not depend
on dwell length. `DEV:HCI_STATUS` again showed zero `command_failures`,
`malformed_events`, and `dropped_events`.

The beacon counter's per-dwell breakdown is the interesting result: of 51
dwells on channel 11, 27 saw zero beacons and 24 saw 1-3 (matching the
~300 ms / ~102.4 ms ≈ 2.9 beacons-per-dwell arithmetic when reception
succeeded), while channels 1 and 6 saw zero beacons in every single one of
their 105 combined dwells across the whole run. That is the pattern a
weak or moderately distant single real AP on channel 11 produces --
roughly half its beacons lost to fading/interference, non-zero counts
clustering near the arithmetic prediction when reception does succeed --
not the pattern a too-short dwell would produce. A dwell that was
structurally too short to catch a present beacon would show a similarly
sparse, low pattern on every channel with an AP, not a clean, sustained
zero on two channels for the entire 46 s run while the third fluctuates.
This is not proof (no reference AP with a known beacon interval was
available to establish ground truth), but it is evidence the 300 ms
margin is doing its job rather than silently starving reception.

This supersedes the 1 s-dwell numbers above for the product-relevant
question ("is the dwell long enough to actually observe beacons"), while
confirming the same conclusion on switching cost and BLE non-interference
at three times the switch rate.

### Second revision: Kismet's two-beacon-interval floor (205 ms)

300 ms (roughly three beacon periods) was itself an arbitrary safety
margin, not a principled floor. Kismet -- widely-deployed prior art for
exactly this problem -- dwells for two beacon intervals per channel before
hopping, on the reasoning that one interval risks landing the entire dwell
window exactly on the gap between two beacons if phase alignment is
unlucky, while two intervals guarantees at least one beacon falls inside
the window regardless of phase. `kChannelDwellMs` was revised again, from
300 to 205 ms (2 x 102,400 us, rounded up to a whole millisecond) --
matching that floor rather than padding past it. `pdMS_TO_TICKS` rounds
this up to 210 ms in practice, at `CONFIG_FREERTOS_HZ`'s 10 ms tick.

Run unattended over 46 s and 235 channel switches at the revised 205 ms
(actual ~210 ms) dwell:

| Metric | Value |
| --- | ---: |
| Channel switches | 235 |
| Min / max switch duration | 546 / 755 μs |
| Average switch duration | 632 μs |
| Revisit period (3 channels x ~210 ms dwell) | ~0.63 s |
| Beacon frames observed (of 87 total management frames) | 54 |

Switch cost is unchanged again, as expected. `DEV:HCI_STATUS` showed zero
`command_failures`, `malformed_events`, and `dropped_events`.

Channels 1 and 6 again saw zero beacons across all 157 combined dwells.
Channel 11's 78 dwells showed a repeating pattern, roughly two nonzero
readings (1 or 2 beacons) followed by two zero readings:
`2 0 0 2 0 0 2 2 0 0 1 0 0 1 1 0 0 1 0 0 0 2 0 0 1 1 0 0 2 0 0 2 2 0 0 2 0
0 1 2 0 0 2 0 0 2 2 0 0 2 0 0 1 2 0 0 2 1 0 0 2 0 0 2 2 0 1 2 0 0 2 2 0 0
2 0 0 2`. This is a different shape than the 300 ms run's roughly-half-hit
pattern, and it is genuinely ambiguous between two explanations this probe
cannot distinguish without a reference AP: continued fading/multipath on
a weak channel-11 AP, or a real AP whose beacon period is longer than the
802.11 default (a period near 3 x this dwell's revisit cadence would
produce exactly this kind of every-third-visit pattern). Either way, the
floor is not silently failing: zero-beacon dwells on channel 11 are
interspersed with real captures, not universal, and the two known-quiet
channels (1, 6) stay at a clean, sustained zero throughout, which remains
the signature a too-short dwell would not produce.

This is now the current dwell.

### Third revision: full US channel plan (1-11)

`kChannelPlan` was widened from the 3-channel (1/6/11) non-overlapping
subset to the full US FCC Part 15 range, channels 1 through 11 -- the
first region-sized set this probe covers, not yet ADR 0026's eventual
per-region Channel Plan (which does not exist yet, and would need its own
selection for non-US regions). Dwell stays at the Kismet-aligned 205 ms
(~210 ms actual) from the second revision above.

Run unattended over 120 s and 609 channel switches:

| Metric | Value |
| --- | ---: |
| Channel switches | 609 |
| Min / max switch duration | 522 / 969 μs |
| Average switch duration | 547 μs |
| Revisit period (11 channels x ~210 ms dwell) | ~2.3 s |
| Beacon frames observed (of 122 total management frames) | 86 |

Switch cost stayed in the same sub-millisecond range as the 3-channel
plans above; the wider plan does not change per-switch cost, only how
often each individual channel is revisited. `DEV:HCI_STATUS` again showed
zero `command_failures`, `malformed_events`, and `dropped_events` across
the full 120 s run.

Per-channel beacon breakdown (55-57 dwells per channel over the run):

| Channel | Dwells with >=1 beacon | Beacons observed |
| --- | ---: | ---: |
| 1-8, 10 | 0 / ~55 | 0 |
| 9 | 27 / 55 | 47 |
| 11 | 22 / 55 | 39 |

This is the practical payoff of covering the full plan rather than the
3-channel subset: channel 9, invisible to every earlier measurement in
this record because it was never in the probed set, turns out to carry
real beacon activity comparable to channel 11's. The earlier 3-channel
runs were not wrong about channels 1/6/11, but they could not have told
this environment's channel-9 AP from channels 2-5/7/8/10 being genuinely
silent, since none of those were ever dwelled on. Channels 1-8 and 10
stayed at a clean, sustained zero across every one of their combined ~495
dwells, consistent with no beacon-emitting AP reaching this board on those
channels in this environment (not a probe fault -- the same clean-zero
signature the earlier 3-channel runs relied on to distinguish real silence
from a too-short dwell holds here too).

This does not establish behavior under simultaneous UI/touch/flash load
(the earlier touch session above ran without channel hopping enabled) or a
non-US regional channel set (12-13, or 12-14 with Japan's channel 14). The
BLE-scan-window side of switching, left open above, is covered next.

## BLE-side switching: scan disable/enable round trip

BLE has no per-channel select command comparable to `esp_wifi_set_channel()`
-- the controller itself cycles the three primary advertising channels
(37/38/39) on its own while a scan is running. The switching-gap question
on the BLE side is therefore different: the cost of stopping and
restarting the scan itself, which an explicit BLE Observation Window (ADR
0003) would need to pay every time it hands airtime to WiFi.

`controller_probe.cpp` gained a `BleScanToggleTask` that cycles LE Set Scan
Enable off then on every 2 s (an arbitrary window -- unlike 802.11's fixed
default beacon period, there is no equivalent standard default BLE
advertising interval to align to; peripherals commonly use anywhere from
~20 ms to several seconds), timing each command's real round trip. Timing
uses a new `SendCommandAndWaitForCompletion` helper that waits for the
actual HCI Command Complete event via a semaphore signaled from the event
parser, rather than `WaitAndSend`'s existing blind 30 ms guess (used
elsewhere for the one-time boot sequence, where a fixed guess was
acceptable because nothing there was being measured). `DEV:BLE_TOGGLE_STATUS`
reports the cumulative toggle count and last/max/average round-trip
duration, alongside a running total of advertising reports seen while the
scan was deliberately disabled -- expected to stay at zero.

Run unattended over 70 s and 17 toggle cycles, concurrently with the
11-channel WiFi hop from the section above (335 WiFi channel switches and
124 beacon frames observed over the same window, both consistent with
that section's per-switch/per-channel figures, confirming the two probes
don't interfere with each other):

| Metric | Value |
| --- | ---: |
| Toggle cycles | 17 |
| Scan disable duration | 466-1128 μs |
| Scan enable duration | 1417-5416 μs |
| Average round trip (disable+enable) | 1364 μs |
| Advertising reports seen while disabled | 0 (all 17 cycles) |

Disabling the scan is cheap and consistent, under 1.2 ms every time.
Enabling it is markedly more expensive and asymmetric: the first two
enables in the run took 5416 and 5330 μs, then settled to roughly
1400-2000 μs for the remaining fifteen. This shape (expensive first,
cheaper once warmed up) suggests some one-time or periodically-refreshed
controller-side setup cost on scan start (comparable in spirit to a
frequency synthesizer settling time or an internal state reset), not a
constant per-call cost -- but that is inference from the shape of the
data, not confirmed against the controller's internals, which are closed.

`DEV:HCI_STATUS` showed zero `command_failures`, `malformed_events`, or
`dropped_events` across the run, and -- the more important number --
**zero advertising reports were counted while the scan was deliberately
disabled, in every one of the 17 cycles**. This is direct evidence that
LE Set Scan Enable's "disable" actually stops reception immediately from
the host's perspective, rather than leaving in-flight reports to trickle
in after the command completes. An explicit BLE Observation Window that
hands the BLE scan window to WiFi can trust that a disable is a real,
clean stop.

This establishes the BLE-side switching cost (sub-2 ms disable, up to
~5.4 ms enable in the worst case observed) and that toggling doesn't
silently leak reception during the "off" state. It does not establish this
under simultaneous UI/touch/flash load, at a shorter or longer toggle
window than 2 s, for active (rather than passive) scanning, or with a real
BLE Observation Window scheduler that alternates BLE and WiFi exclusively
rather than running both continuously as this probe still does (BLE
toggling on/off was tested independently of, not instead of, the
continuous WiFi channel hop above).

## Non-US channel superset: does passive monitoring reach 12-14?

The full US channel plan above leaves open whether channels 12-14 --
needed by any future ETSI (1-13) or Japan (1-14) Channel Plan preset under
ADR 0026 -- are reachable at all with `esp_wifi_set_channel()` under this
project's current configuration, which sets no explicit
`esp_wifi_set_country()` and so runs on ESP-IDF's default country/region
state. **This test, like every other probe in this record, is passive
receive-only**: the WiFi side stays in `WIFI_MODE_NULL` promiscuous
monitor mode (no STA/AP, no association, no probe requests, no
transmission of any kind) and the BLE side stays on passive scan
(`kPassiveScanParameters`, no active scan requests, no connections). It
answers only whether the driver call succeeds and keeps switching
cleanly while listening on 12-14 -- not scheduler behavior, transmit
legality, regulatory correctness, or channel 14's Japan-only DSSS-rate
restriction (which is a transmit-rate restriction and does not bound
passive reception).

`kChannelPlan` was temporarily widened in the working tree (not committed)
to `{1, ..., 14}`, built as the same controller-only-BLE plus
passive-Wi-Fi configuration as the live board result above
(`sdkconfig.defaults` + `sdkconfig.controller_only_probe.defaults` +
`sdkconfig.passive_wifi_probe.defaults`), flashed to the known E32R28T on
`/dev/cu.usbserial-140`, and captured unattended over roughly 45 s (200
channel switches, 14-15 dwells per channel):

| Metric | Value |
| --- | ---: |
| Channel switches | 200 |
| Switches that failed (`channel switch failed` log) | 0 |
| Min / max switch duration (all channels) | 530 / 2301 μs |
| Switch duration into channels 12-14 specifically | 535 μs - 1922 μs |
| Dwells per channel | 14-15 |

Every switch onto 12, 13, and 14 succeeded and cost the same
sub-millisecond-to-low-millisecond range as switches within 1-11 -- no
distinct failure mode or cost cliff at the US/non-US boundary. Advertising
reports and WiFi management-frame counts kept climbing normally throughout
(reaching 421 and 47 respectively by the end of the run), and the board
remained fully responsive and recoverable afterward: reflashed back to the
committed 1-11 plan and reflashed image, it resumed clean operation
immediately with no distinct handling required.

This establishes that channels 12-14 are reachable at the driver level for
**passive listening only** (monitor-mode WiFi sniffing and passive BLE
scanning; no transmission was attempted on any channel), without any
`esp_wifi_set_country()` call, on this ESP-IDF version and board. It does
**not** establish RF receive performance on those channels (this board has
no reference equipment to verify actual receive sensitivity or antenna
behavior at 12-14 versus 1-11), any transmit behavior or legality
(untested and out of scope -- the Monitor Device never transmits), whether
a real EU/Japan Channel Plan preset would need an explicit
`esp_wifi_set_country()` call for correctness or regulatory conformance
(IDF's channel-availability table is still driven by the country setting
even for a receive-only device), or channel 14's Japan-specific
transmit-rate restriction (DSSS-only, which bounds transmission and has no
bearing on passive reception). Those remain open for whichever ticket
implements ADR 0026's actual per-region Channel Plan. The temporary 1-14
plan was reverted before this record was written; the committed probe
still runs the US 1-11 plan documented above.

## One-second serial traffic: a pre-C01 size estimate and live throughput probe

C01 (the real CBOR snapshot contract) has not started, so F08's
"one-second full-snapshot serial bandwidth" item has no real message
sizes to test against yet. [`snapshot-size-estimate.md`](snapshot-size-estimate.md)
works out a representative size ahead of that ticket by costing
[host-metrics.md](../../../../docs/host-metrics.md)'s full metric list in
CBOR against [ADR 0011](../../../../docs/adr/0011-use-versioned-cbor-over-usb.md)'s
envelope: **~1,019 bytes** for a text-encoded snapshot capped at 8
reported interfaces (the dominant, currently-unbounded cost -- see that
note for why, and for a binary-encoding alternative that would roughly
halve it). This supersedes the prior arbitrary 1,024-byte placeholder with
one that has a documented basis, without pretending to be C01's actual
wire format.

[`serial_snapshot_probe.py`](serial_snapshot_probe.py) streams
newline-framed, sequence-numbered filler lines at that size from the host
side; a new `DEV:SERIAL_RX_TEST` dev-console command
(`firmware/platform/esp32/dev_console/dev_console.cpp`) counts bytes
received, detects sequence gaps, and times inter-frame arrival for 60
frames. Both are throughput/loss instrumentation only -- no CBOR is parsed
on either side, since there is no schema yet to parse.

The first run at the designed 1 Hz / 1,019-byte cadence tripped the task
watchdog on `IDLE1` every ~5 s for the whole run: `pdMS_TO_TICKS(5)`
truncates to 0 ticks at this project's 100 Hz tick rate, so the
new command's "no data yet" branch never actually yielded the CPU, in a
task (`dev_console`) that already runs one priority level above idle.
Raising that delay to a real block fixed the watchdog trip but, at 50 ms,
introduced silent byte loss instead (`sequence_gaps=5`, spurious
near-0 ms frame intervals, and leftover stream bytes afterward
misparsed by the ordinary command parser as garbage lines) --
at 115200 baud this console's polling VFS UART driver can have several
hundred bytes waiting by the time a 50 ms-blocked task next polls it, more
than its buffer holds. A 10 ms (1-tick) delay resolved both: real per-poll
blocking without missing bytes.

With that fix, on the plain (non-radio) diagnostic build, idle otherwise:

| Requested rate | Measured throughput | Frames | Bytes | Sequence gaps | Watchdog trips |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 Hz (product cadence) | 918 B/s | 60/60 | 61,200/61,200 | 0 | 0 |
| 5 Hz | 3,274 B/s | 60/60 | 61,200/61,200 | 0 | 0 |
| 50 Hz (host-paced ceiling) | 7,730 B/s | 60/60 | 61,200/61,200 | 0 | 0 |
| Unpaced (`--rate-hz 1000`) | 9,342 B/s | 60/60 | 61,200/61,200 | 0 | 0 |

The unpaced run is close to 115200 baud's 8N1 theoretical ceiling
(~11,520 B/s) and still lost nothing -- the host-side `pyserial` write
call, not the ESP32 side, is the limiting factor at that point. Against
the ~1 KB/s the product actually needs once per second, this is roughly
**9x measured headroom** on the receive side, all zero-loss.

This is a real result on real hardware, but a narrow one: idle-board
byte throughput and loss through the current dev-console recovery path
only (`dev_console.cpp` itself says TB02 replaces this with the negotiated
product command -- a real UART driver with an interrupt-fed ring buffer,
not this polling VFS console, may have different headroom entirely). It
does **not** yet cover the combined WiFi/BLE/touch/flash load the rest of
this record measures separately, real CBOR encode/decode cost on either
side, or C01's actual wire format once it exists. The watchdog/byte-loss
bug this run surfaced was in the throwaway test harness added for this
probe, not in any previously-shipped path.

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
