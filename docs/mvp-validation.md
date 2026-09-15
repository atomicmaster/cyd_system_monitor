<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# MVP delivery and validation

## Milestones

The runnable milestone sequence (M0 through M7, including the early M1a feasibility gate) is defined once, in [IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md), which remains its single source of truth. Every milestone from M1 onward remains runnable on the physical E32R28T; the first release claims no other hardware profile.

## Early feasibility evidence

M1a must record combined radio/display/touch/USB/flash behavior before the full Host application and UI work. Evidence includes receiver switching and coverage measurement limits, packet-processing loss, one-second full-snapshot bandwidth, peak RAM and application size, the display/touch/SD SPI arrangement, and a provisional partition/write budget. The targets of 32 detailed active Alerts, 128 ended/interrupted Alerts, and 512 Transmitter Baselines may change to fit measured safe capacities. M6 validates the resulting enforced limits and journal reclamation before release.

## Simulator

The host-native simulator deterministically exercises protocol negotiation and reconnection, Host Metrics and availability, observation schedules and gaps, radio evidence, Alert episodes and observed-quiet recovery, powered data-link loss, power-loss interruption, wall-clock changes, ordering across reboots, capacity pressure, and corrupt or torn persistence. Scenarios are reusable by domain tests and protocol replay.

An LVGL desktop target consumes the same scenarios for interactive UI review and stable visual fixtures. Simulation is not evidence that timing, RF coverage, storage endurance, touch behavior, or display performance passes on the physical Monitor Device.

## Performance budgets

MVP Host collection targets less than 1% average CPU and 75 MiB resident memory. Apple GPU/ANE diagnostics and their privileged helper are deferred to V2; helper packaging, behavior, and overhead are not MVP release gates.

On the Monitor Device:

- processed touch input produces visible response within 100 ms;
- ordinary animation remains near 20 frames per second;
- stale Host state appears within one snapshot interval of its configured threshold;
- an Alert appears within 500 ms after its qualifying evidence has been processed; and
- UI work never silently reduces radio observation: actual Observation Windows and Coverage Gaps remain measured and visible.

Performance runs state the hardware profile, firmware build, scheduler, radio load, Host build, enabled optional capabilities, and enforced history capacities.

## Routine diagnostics

Host logs rotate at seven days or 50 MiB, whichever is reached first. Firmware keeps a small volatile diagnostic ring while reset reasons and persistence faults use their existing durable records. Routine logs omit radio packet payloads, prompt and source content, and high-rate transmitter observations.

## Release gate

MVP requires all of the following:

- clean install and removal on a fresh supported Apple Silicon Mac;
- first-start hardware verification, five-point touch calibration and validation, and pairing, plus a WiFi Channel Plan preference changeable at any time from Settings;
- every promised Host Metric, availability state, radio view, and Overview interaction;
- immediate complete snapshots after negotiation/reconnect and complete one-second snapshots thereafter;
- Host WiFi coverage states for observable 2.4 GHz, outside-supported-coverage 5/6 GHz, and unknown context, with no inference from a matching SSID alone;
- powered data-link loss, USB power removal/reconnect, Host sleep/wake, daemon restart, incompatible-protocol, and unavailable optional Association Context flows;
- flash-journal recovery after controlled power interruption, including during reclamation, preserving committed records and reporting damaged records;
- prior active episodes marked interrupted after reboot, with fresh evidence starting new episodes and no invented recovery or unpowered-interval observations;
- controlled boundary, minimum-evidence, Confidence, observed-quiet recovery, Coverage Gap, and acknowledgment replay for every shipped Alert rule;
- benign controls for normal roaming, intentional WiFi disconnects, Host sleep/wake, and ordinary BLE address changes, without unsupported hostile-impact claims;
- no Extreme Alerts: that category awaits the relevant detections and sufficient validation data in a later, unassigned release;
- zero known false Medium or High Alerts during a representative 72-hour hardware soak;
- no more than three false Low Alerts per rule during that soak;
- Host and Monitor performance within the stated budgets while coverage is recorded; and
- successful signed update plus recovery of an interrupted or unbootable image through documented BOOT/RESET and the ESP32 ROM serial bootloader.
