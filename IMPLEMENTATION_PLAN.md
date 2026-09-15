<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Implementation plan

This plan turns the accepted product brief into runnable vertical milestones. [docs/mvp-validation.md](docs/mvp-validation.md) is the release gate, [CONTEXT.md](CONTEXT.md) defines domain language, and the ADRs constrain implementation choices.

The product scope is ready for feasibility validation. Its primary goal is a system dashboard with ambient 2.4 GHz WiFi and BLE awareness. Combined hardware performance and retained-history capacities remain unverified; M1a resolves those constraints before the full Host application and UI work.

The [tracer-bullet backlog](docs/tickets/README.md) breaks this plan into owned, dependency-linked tickets. M0–M7 are acceptance milestones, not one-agent assignments or a requirement to finish every layer before exercising a small end-to-end path. M1a remains a hard prerequisite for production protocol/domain and Host/UI slices. After that gate, ticket dependencies control execution and each milestone closes only when its full exit criteria pass on the integrated physical build. The first working Host path is the negotiated CPU snapshot through USB to the display, including stale state and reconnect.

## Working rules

- Keep firmware domain logic independent of ESP-IDF drivers so the host-native simulator exercises the same rules and state transitions.
- Keep Host Metric contracts platform-neutral; isolate macOS collection, permissions, and service management behind adapters. Apple GPU/ANE diagnostics and their privileged helper are V2 work.
- Generate protocol types and fixtures from one language-neutral schema. Firmware and Host must pass the same golden vectors.
- Put all E32R28T pins and capabilities in its hardware profile. Application modules consume capabilities rather than board-name branches.
- Treat radio frames, BLE labels, USB messages, persisted records, imported configuration, and downloaded manifests as untrusted bounded input.
- Preserve availability, stale time, Observation Windows, and Coverage Gaps through every aggregation and display path.
- Keep each milestone usable on the physical E32R28T before advancing the release branch.

## M0 — Repository and reproducible toolchains

Create the planned `firmware/`, `host/`, `macos/setup-app/`, `protocol/`, and `simulator/` seams. Pin ESP-IDF, LVGL, Rust, Swift, formatting, and schema-generation versions. Add license identifiers, dependency notices, deterministic developer setup, and CI builds for firmware, Rust, Swift, protocol fixtures, and host-native tests.

**Exit criteria:** a clean Apple Silicon macOS 26 machine can bootstrap the toolchains, build empty runnable targets, run formatting/tests, and verify the E32R28T profile without depending on ignored vendor downloads.

## M1 — E32R28T bring-up

Implement profile-driven display, backlight, XPT2046 touch, RGB LED, audio enable, MicroSD probe, battery-ADC diagnostic, UART, and reset-reason drivers. Build the five-point calibration and independent validation flow, NVS schema, landscape coordinate mapping, settings recovery, and USB calibration reset.

Exercise every vendor-claimed pin on the connected board and record measured exceptions in the hardware profile. Verify that GPIO4 low enables the fitted audio-amplifier behavior without assuming undocumented electrical equivalence.

**Exit criteria:** the real board boots reliably, validates its profile, renders a diagnostic screen, passes touch calibration and validation after power loss, exercises LEDs and optional sound, reports storage and reset state, and can be recovered with BOOT/RESET.

## M1a — Combined hardware feasibility gate

Exercise minimal passive WiFi capture and BLE scanning alongside representative LVGL rendering, touch input, USB snapshot-sized traffic, and flash writes on the physical E32R28T. This is a bounded feasibility milestone; the production scheduler, parsers, and rules remain in M5/M6.

Measure switching overhead, channel revisit timing, observation coverage and its measurement limits, packet-processing loss, touch latency, animation performance, peak RAM use, and firmware size. Distinguish requested Observation Windows from verified receiver availability; expose uncertainty or loss rather than treating scheduled time as guaranteed reception. Check one-second full-snapshot serial bandwidth with representative maximum-sized messages.

Resolve the display/touch/MicroSD bus allocation: the board wires three separate SPI buses, while ESP32 has two general-purpose SPI controllers. Evaluate software-driven touch SPI or another measured arrangement, including its effect on responsiveness. The hardware profile documentation records the chosen arrangement.

Produce a provisional 4 MB partition layout and RAM budget covering the application, display and radio buffers, settings, maximum record sizes, journal reclamation space, checkpoint/write rates, and reserved Operational and Capacity-pressure Alert storage. The targets of 32 detailed active Alerts, 128 ended/interrupted Alerts, and 512 Transmitter Baselines may be adjusted to measured safe capacities; they are not fixed release promises. Record headroom and the write/endurance assumptions to validate in M6/M7.

**Exit criteria:** a recorded combined-load run demonstrates a viable path to the MVP performance budgets, documents coverage limitations, establishes a workable SPI arrangement, and supports explicit memory/storage budgets. Resolve scope or capacity changes before advancing to M2; do not proceed on an assumption that the combined workload fits.

## M2 — Protocol core and deterministic simulator

Define framing, CBOR schema, version negotiation, capability exchange, Pairing Identities, sequence handling, time synchronization, configuration revisions, complete one-second snapshots plus an immediate snapshot after session negotiation, commands, errors, and forward-compatible optional fields. MVP has no incremental Host Metric updates. Create golden byte vectors, malformed-input cases, a desktop decoder, and replayable scenario files.

Implement the shared domain model for availability, stale state, observation schedules, coverage, Alert episodes, acknowledgment, capacity pressure, reconnects, Time Discontinuities, and persistence recovery. Feed it through a deterministic host-native simulator and an LVGL desktop target.

Before fixing each rule's scenario expectations, document its evidence eligibility, minimum Observed Time and evidence counts, Confidence criteria, and observed-quiet recovery behavior. Numeric thresholds remain candidates until M5/M6 measurements. Build the shared interfaces incrementally as tracer bullets require them; M2 closes only after all its domain/replay acceptance is demonstrated. Separate Metric Availability, freshness, and adapter enablement. Specify persistent evidence ordering across boot identities and interruption recovery without inventing timestamps or observations across power loss.

**Exit criteria:** firmware-side and Rust-side codecs accept every golden vector, reject bounded malformed inputs, negotiate supported versions, report incompatible majors, replay the same scenarios to equivalent domain states, and recover cleanly across simulated reconnects and time jumps.

## M3 — macOS metrics and USB appliance loop

Implement the per-user Rust LaunchAgent, CLI, USB discovery, confirmed one-to-one pairing, fresh-session resynchronization, periodic snapshots, time uncertainty, and Host Last Seen behavior. Add Mach CPU, Physical RAM Occupied, root filesystem/device, primary-route and per-interface network metrics, public battery data, defensive slow battery-condition collection, and exact same-user agent-process presence.

Add the SwiftUI status/permission app and optional Location workflow for Association Context. Report Host WiFi band/channel when available so firmware can distinguish observable, outside-supported-coverage, and unknown context. GPU/ANE collection and privileged-helper installation are outside MVP.

**Exit criteria:** the physical Monitor receives complete one-second snapshots and an immediate fresh snapshot after reconnect, distinguishes every Metric Availability state, survives sleep/wake and daemon restart, dims stale metrics correctly, continues without optional Association Context, and stays within the Host resource budget.

## M4 — MVP display interface

Implement the persistent status bar, six-card Overview, expanded current-detail screens, Alert list/detail/acknowledgment, Settings, About, pairing, first-start flow, disabled-radio states, Host Last Seen, and `Time unavailable`. Show outside-supported-coverage and unknown Host WiFi context, interrupted Alert episodes, and temporarily unassessable active episodes during Coverage Gaps. Apply an accessible dark palette with redundant icon/text/shape meaning and reserve Severity colors for Alerts.

Use the LVGL desktop target for scenario coverage and visual fixtures, then verify touch target size, readability, response, animation, and navigation on the 320×240 display.

**Exit criteria:** every accepted UI state is reachable through deterministic scenarios, no missing value appears as zero, persistent status remains visible, the physical display meets touch and animation budgets, and Overview requires no scrolling or graph rendering.

## M5 — Passive WiFi and BLE observation

Build on M1a measurements to implement explicit 2.4 GHz WiFi channel and BLE Observation Windows, the default balanced scheduler, a configurable WiFi Channel Plan, measured coverage, and independent radio controls. Produce bounded WiFi BSS, frame-type, transmitter, frame-rate, estimated-observed-airtime, BLE advertisement-density, Signal Strength, and common-beacon summaries without retaining raw payloads. The board cannot observe 5/6 GHz Host traffic; matching SSIDs on 2.4 GHz do not supply that missing evidence.

Tune dwell and revisit timing on the real board under idle, busy UI, USB traffic, and dense radio conditions. Record actual coverage and CPU/memory pressure; do not infer quiet conditions from unsampled intervals.

**Exit criteria:** observation continues without Host software or a data connection while power remains available, both radios can be disabled independently, schedule changes end affected episodes correctly without creating Alerts, displayed claims match captured evidence, and hardware measurements establish conservative scheduler defaults.

## M6 — Alert rules and durable radio state

Implement versioned coverage-normalized rules for deauthentication/disassociation rate, BLE advertisement flooding, and identifier churn. Add Low/Medium/High Severity, independent Confidence, relevant Host context and impact evidence, first/last/peak evidence, observed-quiet recovery, acknowledgment, provenance, aggregate Capacity-pressure Alerts, and bounded Alert/Baseline retention using the capacities established in M1a. Extreme is not emitted in MVP.

Implement NVS settings/calibration plus the CRC-protected append journal, schema migration, corruption recovery, separate history erase/factory reset, and explicit raw-address export. On reboot, preserve committed evidence and mark previously active episodes interrupted; fresh qualifying evidence starts new episodes. Validate journal reclamation and write budgets. Generate boundary and benign-event fixtures before tuning thresholds against controlled traffic and ordinary desk recordings.

**Exit criteria:** every rule triggers at its documented boundary, gaps pause Observed Time and recovery evaluation, acknowledgment never ends an episode, Extreme is never emitted, power cuts preserve committed records and report interruption/loss without claiming recovery, hostile identifier volume remains bounded, and configuration changes end and reset affected evidence without emitting an Alert.

## M7 — Release engineering and qualification

Build the signed/notarized macOS package, LaunchAgent lifecycle, explicit GitHub Release check/fetch, signed manifest, verified USB flashing, diagnostics bundle, configuration backup/import, and separated uninstall/erase operations. Produce complete source, license, notice, compatibility, update, and BOOT/RESET recovery material. Privileged-helper packaging and lifecycle belong to V2.

Run clean-machine installation/removal, protocol compatibility, interruption recovery, performance, controlled Alert replay, benign roaming/disconnect/sleep/wake/BLE-address-change scenarios, hardware smoke, and the representative 72-hour soak. Distinguish powered data-link loss from removal of the sole USB power cable. Record the exact firmware, Host, protocol, rules, hardware profile, scheduler, capacities, permissions, and optional capabilities used.

**Exit criteria:** every item in [docs/mvp-validation.md](docs/mvp-validation.md) passes, including zero known false Medium/High Alerts, at most three false Low Alerts per rule, and successful recovery from an interrupted or unbootable firmware image.

## Deferred work

V1 begins only after the MVP gate and adds SQLite Host History plus device radio aggregates and graphs at the accepted retention tiers. V2 adds Apple GPU/ANE diagnostics and their optional restricted privileged helper, Linux, Windows, opt-in deep Agent Adapters, the Host Device Manager, GPS ingestion, and SD-backed WiGLE Survey Artifacts. Advanced radio rules remain research-gated. Extreme Severity is deferred to the release in which all detections relevant to its claim are implemented and sufficient validation data supports escalation; neither V1 nor V2 promises it. That release must define corroboration and timing criteria and include Extreme in its false-positive gate. Incremental Host Metric updates require measured justification before being reconsidered.
