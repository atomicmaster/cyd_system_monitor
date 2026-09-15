<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# CYD System Monitor

CYD System Monitor is a local desk appliance for a verified LCDWiki E32R28T touch display. It presents live metrics from one paired Apple Silicon Mac and independently observes nearby 2.4 GHz WiFi and Bluetooth Low Energy activity using the display's ESP32 radio.

The product scope is ready for feasibility validation. An early combined hardware milestone will establish radio/display performance and safe memory and storage capacities before the full Host application and UI work. The accepted product brief is in [BRAINSTORM.md](BRAINSTORM.md), the implementation sequence is in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md), and the domain glossary is in [CONTEXT.md](CONTEXT.md).

## MVP boundary

- LCDWiki E32R28T touch display in 320×240 landscape orientation
- Apple Silicon Host running macOS 26 or newer
- Rust Host daemon and CLI plus SwiftUI setup/status application
- ESP-IDF C++ firmware with LVGL
- complete one-second Host Metric snapshots over versioned CBOR on physical USB
- passive scheduled 2.4 GHz WiFi and BLE observation with visible coverage
- deterministic evidence-backed Low/Medium/High Alerts and device-owned Radio History
- first-start touch calibration and one-to-one Host pairing, with a configurable WiFi Channel Plan available anytime in Settings
- no background network traffic; explicit release checks and downloads use GitHub Releases

The primary goal is a system dashboard with ambient radio awareness. The board cannot observe 5/6 GHz WiFi; Host-targeted radio correlation requires relevant captured evidence. Standalone radio monitoring continues without Host software or a data connection only while power remains available. GPU/ANE diagnostics and the privileged helper move to V2; Extreme Severity awaits the relevant detections and sufficient validation data, with no version assigned.

See [docs/roadmap.md](docs/roadmap.md) for MVP, V1, and V2 scope and [docs/mvp-validation.md](docs/mvp-validation.md) for the release gate.

## Documentation

| Area | Document |
| --- | --- |
| Product roadmap | [docs/roadmap.md](docs/roadmap.md) |
| Agent ticket backlog and dependencies | [docs/tickets/README.md](docs/tickets/README.md) |
| Host metrics | [docs/host-metrics.md](docs/host-metrics.md) |
| Radio evidence and Alerts | [docs/radio-model.md](docs/radio-model.md) |
| Display interface | [docs/ui-design.md](docs/ui-design.md) |
| Pairing, time, and recovery | [docs/device-lifecycle.md](docs/device-lifecycle.md) |
| macOS components | [docs/macos-components.md](docs/macos-components.md) |
| Agent integrations | [docs/agent-integrations.md](docs/agent-integrations.md) |
| GPS/WiGLE surveys | [docs/gps-surveys.md](docs/gps-surveys.md) |
| Repository seams | [docs/repository-layout.md](docs/repository-layout.md) |
| Architectural decisions | [docs/adr](docs/adr) |
| Active hardware profile | [hardware/profiles/lcdwiki-esp32-32e-2.8](hardware/profiles/lcdwiki-esp32-32e-2.8) |

## Hardware material

The active unit was probed as an ESP32-D0WD-V3 revision 3.1 with 4 MB flash and a CH340/CH341 USB serial bridge. The hardware profile contains project-authored configuration and development notes. Downloaded vendor manuals, datasheets, archives, photos, and design files remain in an ignored local cache because no redistribution license was found.

## Licensing

Project-authored software is licensed under GNU GPL version 3 only (`GPL-3.0-only`), except the firmware image and any `protocol/` code compiled into it, which use Apache License 2.0 (`Apache-2.0`) to stay coherent with Espressif's own object-only radio-stack libraries — see [ADR 0027](docs/adr/0027-license-firmware-under-apache-2-0.md). Project-authored documentation is licensed under Creative Commons Attribution-ShareAlike 4.0 (`CC-BY-SA-4.0`). Third-party components and materials retain their original licenses and notices. See [LICENSE](LICENSE), [LICENSES](LICENSES), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
