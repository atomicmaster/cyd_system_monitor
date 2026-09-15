<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Planned repository layout

```text
/
├── firmware/                 ESP-IDF C++ application and host-native domain tests
├── host/                     Rust workspace: daemon, CLI, platform adapters; helper in V2
├── macos/setup-app/          SwiftUI setup/status app; local Device Manager in V2
├── protocol/                 schema, compatibility rules, golden vectors, decoder
├── simulator/                synthetic Host Metrics and Radio Observation replay
├── hardware/profiles/        isolated board profiles and local vendor caches
└── docs/                     product design and architectural decisions
```

The protocol schema and golden vectors are language-neutral inputs to both runtimes. Hardware-specific pins, capabilities, setup, and smoke tests stay under a profile. Product concepts and radio rules remain independent of the E32R28T so another profile, including a round display, can reuse them with a different layout and input adapter.

Configuration and persistence records are versioned. A configuration update names the revision it read; stale touch or USB writes are rejected and reloaded. Changing a WiFi Channel Plan, scheduler, or alert threshold ends affected Alert episodes with reason `configuration changed`, resets their evidence windows, and starts new episodes if the condition persists. Every Alert retains its rule and firmware versions, effective thresholds, relevant channel-plan/scheduler version, coverage summary, and persistence schema.

Portable configuration export/import is a Host tool with schema validation and a preview before applying changes; an imported WiFi Channel Plan applies like any other portable setting, with no separate reconfirmation step, since it carries no legal-compliance meaning. The export excludes hardware-profile state, touch calibration, Pairing Identities, retained history, and Survey Artifacts. Diagnostic bundles likewise use a versioned schema, omit secrets, and redact radio addresses unless the Operator explicitly includes them.

Repository licensing uses machine-readable SPDX identifiers. Project-authored software is `GPL-3.0-only`, except the firmware image and any `protocol/` code compiled into it, which are `Apache-2.0`; project-authored documentation is `CC-BY-SA-4.0`. Third-party notices and license texts remain with dependencies. Ignored LCDWiki archives, datasheets, photos, and design files are not relicensed or included in source releases without explicit redistribution permission.
