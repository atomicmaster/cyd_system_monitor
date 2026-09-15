<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Product roadmap

## MVP

The MVP is a stable end-to-end one-host system dashboard with ambient 2.4 GHz WiFi and BLE awareness:

- ESP-IDF C++/LVGL firmware for the E32R28T hardware profile;
- a Rust daemon/CLI for Apple Silicon Macs running macOS 26 or newer and a minimal SwiftUI permission/status application;
- complete one-second Host Metric snapshots, plus an immediate snapshot after reconnect, over versioned CBOR/USB;
- a graph-free Overview using dials, state animation, and concise values, plus expanded current-detail screens;
- passive scheduled 2.4 GHz WiFi and BLE observation with disclosed coverage;
- Observed BSS, frame-type, BLE advertisement, Signal Strength, and documented beacon-format views;
- deterministic deauthentication/disassociation, BLE flood, and identifier-churn Alerts using Low/Medium/High Severity;
- bounded Radio History and explicit export, with standalone monitoring while power remains available;
- first-start touch calibration, with a configurable WiFi Channel Plan available anytime from Settings; and
- simulator/replay, protocol vectors, journal recovery, hardware smoke, and 72-hour soak gates.

The board cannot observe 5/6 GHz WiFi. Host-targeted radio correlation requires relevant captured evidence; outside-supported-coverage and unknown Host WiFi context remain explicit states. Normal USB Host Metrics remain available regardless of the Host WiFi band.

The initial verified Host environment is Apple Silicon running macOS 26.6.2. Intel Macs and older macOS releases are outside the MVP support matrix.

Normal operation has no outbound network traffic. The only product-initiated exception is an explicit Operator action to check for or fetch the latest project release from GitHub Releases. Fetching does not install Host software or flash firmware automatically.

Project-authored software will be released under the OSI-approved GNU GPL version 3 only (`GPL-3.0-only`) so commercial use remains permitted while redistributed modifications and binaries retain copyleft obligations, except the firmware image and any `protocol/` code compiled into it, which use the OSI-approved Apache License 2.0 (`Apache-2.0`) because that copyleft duty cannot be met for Espressif's object-only radio-stack libraries the firmware links against. Project-authored documentation will use Creative Commons Attribution-ShareAlike 4.0 (`CC-BY-SA-4.0`). Files will carry SPDX identifiers, dependencies will retain their upstream notices, and cached vendor material will be excluded from the project license and source distribution.

Advanced evil-twin, Karma, forced-handshake, PMKID/EAPOL, cloned-tracker, skimmer, and peripheral-impersonation ideas are research candidates. Each needs an evidence model, honest claims review, controlled trigger tests, and a false-positive target before assignment to a release.

Implementation proceeds as runnable vertical milestones: E32R28T display/touch bring-up; combined radio/display/touch/USB feasibility and RAM/flash budgeting; shared protocol and simulator; macOS Host Metrics over USB; Overview and expanded current-detail UI; scheduled WiFi/BLE observation; Alert rules and persistence; then packaging, recovery, and soak validation. The E32R28T touch profile is the only hardware supported by the first release. The early feasibility gate establishes safe capacities; 32 detailed active Alerts, 128 ended/interrupted Alerts, and 512 Transmitter Baselines are provisional targets rather than fixed release requirements.

## V1

V1 adds interactive historical graphs and time compression. The daemon owns bounded Host Metric time series in a per-user SQLite database using WAL, versioned migrations, and transactional compaction. It retains one-second samples for 15 minutes, 10-second buckets through four hours, one-minute buckets through seven days, and five-minute buckets through 30 days.

The Monitor Device owns radio history, retaining five-second aggregates for 15 minutes in RAM, one-minute aggregates through six hours in flash, and 15-minute aggregates through seven days in flash. Alert evidence has a separate storage reservation and cannot be evicted by graph history; final byte capacities must be verified against the firmware partition layout. Graphs use labeled resolution tiers rather than an unlabeled logarithmic axis. Every compressed bucket preserves minimum, maximum, mean, last value, sample count, and unavailable/stale duration; counter buckets preserve appropriate totals and rates.

Advanced radio rules may enter V1 individually only after satisfying their research and evidence gates; V1 does not promise the entire brainstorm list. Extreme Severity has no assigned version: it belongs to the release where all detections relevant to its claim are implemented and sufficient validation data supports escalation. It is not automatically included in V1 or V2, and its eventual release gate must explicitly cover false Extreme Alerts.

## V2

V2 adds optional Apple GPU and Neural Engine Activity Diagnostics and the restricted privileged helper, including its installation, removal, status UI, and separate overhead validation. These are not MVP or V1 delivery requirements.

V2 adds Linux Host support first, followed by Windows after the platform-adapter boundary has been exercised by macOS and Linux. It also adds deeper, explicitly enabled agent-specific activity and usage adapters and optional GPS-assisted wardriving/network mapping. Agent adapters use documented local APIs, structured logs, or agent-provided hooks and declare the fields they read and retain. They do not inspect conversation files, prompts, source contents, command arguments, or credentials.

Because the E32R28T USB-C connector is a USB device port rather than a USB host, an optional GPS receiver attaches to the Monitored Host. A Rust NMEA 0183 serial adapter sends timestamped Position Fixes to the Monitor Device. The Monitor Device owns each explicitly started Survey Session and records selected WiFi, BLE, or both observation types to SD card. A normal Radio History continues independently when a Position Fix is missing or fails the default five-second age or 50-meter accuracy gate.

Each completed session produces immutable WiGLE Wireless CSV 1.6 `.csv.gz` artifacts plus a sidecar index containing times, bounds, row count, size, SHA-256, and transfer state. Parts rotate at 128 MiB compressed, 24 hours, or a UTC date boundary. An interrupted append-only `.partial` file is validated and finalized during recovery. The Host application lists artifacts and retrieves their original bytes using resumable, checksum-verified transfer. It may link to WiGLE's manual upload page but does not upload automatically or store WiGLE credentials. First recording and first download disclose that exact coordinates and times are paired with SSIDs, MAC addresses, BLE addresses, and decoded labels. Device and Host copies have independent explicit deletion controls.
