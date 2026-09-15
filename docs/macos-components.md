<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# macOS component model

The MVP supports Apple Silicon Macs running macOS 26 or newer. The initial verified environment is macOS 26.6.2 on ARM64. Intel Macs and older macOS releases are outside the supported matrix, although platform-neutral Rust modules may still compile elsewhere.

The MVP contains two narrowly separated components:

1. A per-user Rust LaunchAgent collects ordinary Host Metrics and Agent Activity, owns the USB link, and continues without elevated privileges or WiFi identity permission.
2. A minimal signed SwiftUI setup application uses Apple's CoreLocation, CoreWLAN, and ServiceManagement APIs to explain and optionally request macOS Location Services for SSID/BSSID context. It displays daemon and permission status but is not a monitoring dashboard. In V2 it becomes a small local Device Manager for setup, versions, firmware updates, diagnostics, and Survey Artifact download and deletion while live monitoring and graphs remain on the Monitor Device.

Release installation and removal use a signed, notarized package. Optional Association Context permission or data absence never blocks the per-user daemon.

## V2 privileged diagnostics

V2 adds an optional Rust root helper for Apple GPU/ANE Activity Diagnostics, with helper status in the setup application. It runs only Apple's fixed `powermetrics` samplers and publishes sanitized data over restricted local IPC. It shares diagnostic data types and plist fixtures with the daemon, not a general command runner. Helper installation and removal require explicit administrator authorization; development uses documented `sudo` install/uninstall commands. Helper absence or failure never blocks ordinary collection. Helper implementation, packaging, lifecycle, and overhead validation are not MVP requirements.

## WiFi identity disclosure

Before requesting Location permission, the setup application explains:

> macOS requires Location Services to reveal the connected WiFi SSID/BSSID. This improves correlation of wireless Alerts. CYD System Monitor does not request coordinates, track movement, or store location. You may skip this; monitoring continues with reduced association context.

Permission and data availability are separate. The daemon preserves all Association Context Availability states, including authorized but unavailable, because current macOS versions may return no SSID/BSSID even after consent.

Without permission, the daemon still obtains the active WiFi interface's current `AF_LINK` address and link state. This is the private station address visible over the air, unlike the factory address returned by some CoreWLAN properties. Firmware can correlate direct deauthentication evidence with this Host Station Identity and can infer a Host-associated BSS from passively received frames.

Host WiFi band/channel context, when available, also lets the display show that a 5/6 GHz connection is outside this board's supported radio coverage. Unknown context remains unknown; permission alone does not establish radio coverage.

Future Linux and Windows adapters implement the same optional capability with native permission and availability states. No platform may invent or retain stale association identity when current access is unavailable.

## Local lifecycle tools

The CLI and setup application list compatible attached monitors by Pairing Identity, guide the matching-code confirmation, and make unpairing explicit. After reconnection, they negotiate a fresh protocol session and send an immediate complete Host snapshot, followed by complete snapshots every second. MVP has no incremental Host Metric updates. The Host also supplies UTC, timezone, and clock uncertainty at handshake and periodically afterward.

Diagnostic export is an explicit local action. A bundle may include component versions, negotiated capabilities, reset reasons, permission and storage states, recent bounded logs, and configuration with secrets omitted. Radio addresses are redacted by default and require an explicit `include raw addresses` choice.

Routine Host logs rotate after seven days or 50 MiB, whichever limit is reached first. Device diagnostics use a small volatile ring; reset reasons and concrete persistence faults survive through their existing durable records. Routine logs exclude radio packet payloads, prompts, source contents, and high-rate transmitter observations.

The installed product makes no background network requests. Its only product-initiated outbound operation is an Operator-requested check for or fetch of the latest project release from GitHub Releases, including the redirects needed to retrieve its signed manifest and artifacts. Monitoring, diagnostics, pairing, configuration, and firmware operation remain local. A downloaded release is verified before use and is never installed or flashed without a separate explicit action.

Software removal, per-user Host History and configuration removal, V2 privileged-helper removal, unpairing, and connected Monitor Device erasure are separate actions with their scope explained before execution. Removing Host software does not erase device-owned Radio History. A versioned settings backup may carry portable display, scheduler, WiFi Channel Plan, and rule settings after validation and preview. The backup excludes touch calibration, hardware and Pairing Identities, history, and Survey Artifacts.
