<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Resolved product brief

This document began as the September 15, 2026 brainstorm for an ESP32-32E system and radio monitor. The planning review established the scope below; combined hardware feasibility and numeric resource budgets remain to be validated. Detailed behavior lives in the linked design documents and ADRs; [CONTEXT.md](CONTEXT.md) defines the project vocabulary.

## Product

CYD System Monitor is a local desk appliance for one technical Operator. A verified LCDWiki E32R28T touch display presents current state from one paired Apple Silicon Mac while its ESP32 independently observes the nearby 2.4 GHz WiFi and BLE Radio Environment. The primary goal is a system dashboard with ambient awareness. Host software or its data connection may stop without stopping radio observation or Alert evaluation while the Monitor remains powered. Removing the sole USB power cable stops monitoring; committed Radio History survives restart.

The board cannot observe 5/6 GHz WiFi. Host-targeted radio correlation is conditional on relevant captured evidence, and outside-supported-coverage or unknown Host WiFi context is visible rather than presented as quiet conditions.

The product reports measured evidence and availability honestly. It does not claim continuous radio coverage, physical-device identity, distance, exact RF occupancy, or a confirmed attack when its inputs cannot support those conclusions.

## MVP

The first release includes:

- ESP-IDF C++/LVGL firmware for the E32R28T touch profile in 320×240 landscape;
- first-start five-point touch calibration and validation and basic settings, with a configurable WiFi Channel Plan available anytime;
- a Rust daemon and CLI for Apple Silicon on macOS 26 or newer;
- a signed SwiftUI setup/status application for pairing, service state, updates, and optional Location permission;
- complete one-second snapshots of CPU, RAM, root storage, disk-I/O, network, battery, and basic Agent Activity, with individual source ages and an immediate snapshot after reconnect;
- basic Codex, Claude, and Grok Build process presence, with stronger activity states only when a trustworthy adapter supplies them;
- passive 2.4 GHz WiFi and BLE Observation Windows with current mode and Coverage Gaps always visible;
- WiFi BSS and frame summaries, Observed WiFi Activity, BLE advertisement density, Signal Strength ordering, and safe beacon-format decoding;
- deterministic deauthentication/disassociation, BLE flood, and identifier-churn Alert rules;
- separate Low, Medium, and High Severity plus independent Radio Alert Confidence;
- bounded power-loss-recoverable Radio History and Transmitter Baselines in internal flash;
- a graph-free six-card Overview, expanded current-detail screens, persistent clock/Alert/connection/coverage status, and an About screen;
- independent WiFi and BLE enable controls that do not create Alerts;
- versioned CBOR communication over USB, one-to-one confirmed pairing, reconnect resynchronization, and honest time uncertainty;
- signed firmware retrieval and flashing over USB with ESP32 ROM bootloader recovery; and
- an early combined hardware feasibility gate, deterministic simulation, hardware smoke tests, performance budgets, and a 72-hour release soak.

History capacities are provisional targets: 32 detailed active Alerts, 128 ended/interrupted Alerts, and 512 Transmitter Baselines. The early gate establishes safe byte capacities, RAM headroom, journal reclamation space, and write budgets before those counts become release requirements.

The only product-initiated external communication is an explicit Operator request to check for or fetch the latest GitHub Release. Fetching never installs or flashes automatically.

## Alert claims

Radio Alerts retain evidence, Confidence, Severity, rule and firmware versions, effective thresholds, scheduler and coverage context, and episode times. Acknowledgment marks an Alert as seen without ending or deleting it. Coverage Gaps neither prove quiet conditions nor advance Observed Time or recovery evaluation. Episodes end after sufficient observed quiet time; gaps leave their current condition unassessable. Reboot recovery preserves committed evidence and marks previously active episodes interrupted, with fresh qualifying evidence starting new episodes.

MVP emits only Low, Medium, or High. A deauthentication anomaly may become High when fresh relevant evidence targets the Host Station Identity or Host-associated BSS. Host-impact evidence may be retained but never escalates to Extreme. Extreme is deferred to the release where all detections relevant to that claim are implemented and sufficient validation data supports it; no version is assigned.

Operational Alerts report concrete compatibility, radio, persistence, crash, or active-survey storage failures and do not carry Confidence. Host disconnection, optional capability absence, denied Location permission, and Operator-disabled radios remain statuses.

## Interface

Overview is the primary screen. Its System, Storage, Network, Agents, WiFi, and BLE cards use dials, animated icons, and concise numbers. Tapping a card opens its detail screen. The clock, highest unacknowledged Alert/count, Host/USB state, and active Observation Window persist across screens.

Stale Host indicators dim, their motion decays to neutral, and their final numeric values remain labeled `Last`. Host Last Seen stays visible. The display never converts missing or stale data into a measured zero. It shows `Time unavailable` after cold boot until a trustworthy time source arrives.

## Later releases

V1 adds Host and radio graphs with bounded multiresolution retention. Host History keeps one-second data for 15 minutes, 10-second buckets through four hours, one-minute buckets through seven days, and five-minute buckets through 30 days. Device radio aggregates retain five-second data for 15 minutes in RAM, one-minute buckets through six hours, and 15-minute buckets through seven days in flash, without consuming Alert-evidence storage.

V2 adds Apple GPU/ANE diagnostics through an optional restricted privileged helper, Linux support followed by Windows, opt-in documented agent adapters, and GPS-assisted wardriving. A USB GPS receiver attaches to the Host, while firmware owns explicit Survey Sessions and immutable WiGLE Wireless CSV 1.6 artifacts on MicroSD. The Host Device Manager downloads original artifacts with resumable checksum verification and leaves WiGLE upload to the Operator.

Advanced evil-twin, Karma, forced-handshake, PMKID/EAPOL, cloned-tracker, skimmer, and peripheral-impersonation ideas remain research candidates. None enters a release without an honest evidence model, controlled trigger tests, and an accepted false-positive target.

## Project policy

Project software uses `GPL-3.0-only`, except the firmware image and any `protocol/` code compiled into it, which use `Apache-2.0` to stay coherent with Espressif's own object-only radio-stack libraries; project documentation uses `CC-BY-SA-4.0`. Commercial use is allowed under all these terms, covered redistributed modifications to `GPL-3.0-only` components retain their copyleft obligations, and all third-party licenses and notices remain intact. Cached vendor files without documented redistribution permission are excluded from source releases.

Implementation follows [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). Scope and acceptance changes must update the relevant design document and add an ADR only when the decision is costly to reverse or otherwise surprising.
