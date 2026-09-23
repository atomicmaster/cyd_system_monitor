<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# macos/setup-app

Development-build SwiftUI status/setup app (TB08). Not a dashboard: it shows the
LaunchAgent state, the daemon's status (read from its control socket), pairing
state and the optional WiFi permission state, and hosts the WiFi-context
provider client. It never requests Location permission at launch: the request
sits behind the verbatim disclosure. The daemon does not depend on it.

```text
Sources/SetupCore   pure logic (Swift Testing tests): control models, permission
                    mapping, provider protocol/session/runner, bounded socket
                    client, status text
Sources/SetupApp    SwiftUI shell + CoreLocation/CoreWLAN sensor (not unit-tested)
Tests/SetupCoreTests
```

```sh
./dev build macos
./dev check macos      # swift build + swift test (XCTest needs Xcode; Swift Testing does not)
./dev run macos        # opens the window and starts the provider client
```

`./dev run macos` reads WiFi state and connects to the daemon's sockets; it does
not request Location permission unless you press the button. Contracts:
[host-control-ipc.md](../../docs/host-control-ipc.md),
[wifi-context-provider.md](../../docs/wifi-context-provider.md),
[macos-components.md](../../docs/macos-components.md).

## Signing and consent (Operator; R01 for release)

A bare SwiftPM executable has no bundle or `Info.plist`, so macOS will not show a
Location prompt for it. To exercise the real permission, package it as a signed
`.app` with `NSLocationWhenInUseUsageDescription` (and whatever entitlement the
signing setup requires), then click the button once and answer the prompt.
Notarization, the product identifier and the installer are R01. See
[TB08](../../docs/tickets/TB08.md#remaining-human-and-physical-steps).
