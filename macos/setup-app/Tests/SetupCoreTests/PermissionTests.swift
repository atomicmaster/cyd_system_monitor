// SPDX-License-Identifier: GPL-3.0-only

import Foundation
import Testing

@testable import SetupCore

@Test func disclosureIsVerbatimFromTheComponentDocument() throws {
    // Tests/SetupCoreTests/ -> macos/setup-app -> repository root.
    let root = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    let doc = try String(contentsOf: root.appendingPathComponent("docs/macos-components.md"), encoding: .utf8)
    #expect(doc.contains("> " + Disclosure.text))
}

@Test func locationAuthorizationMapsToTheWirePermissionStatus() {
    let cases: [(LocationAuthorization, Bool, PermissionStatus)] = [
        (.notDetermined, true, .notRequested),
        (.restricted, true, .restricted),
        (.denied, true, .denied),
        (.authorizedAlways, true, .granted),
        (.authorizedWhenInUse, true, .granted),
        // Location Services switched off system-wide: nothing can be revealed.
        (.authorizedWhenInUse, false, .denied),
        (.unknown, true, .error),
    ]
    for (authorization, enabled, expected) in cases {
        #expect(permissionStatus(for: authorization, locationServicesEnabled: enabled) == expected)
    }
}

@Test func contextAvailabilityKeepsPermissionAndDataSeparate() {
    let ssid = WifiSample(band: .ghz5, channel: 36, ssid: [0x61], bssid: nil)
    let bssidOnly = WifiSample(band: .ghz2_4, channel: 6, ssid: nil, bssid: "aa:bb:cc:dd:ee:ff")
    let neither = WifiSample(band: .ghz5, channel: 44)
    let emptySSID = WifiSample(ssid: [])
    #expect(contextAvailability(permission: .granted, read: .sample(ssid)) == .available)
    #expect(contextAvailability(permission: .granted, read: .sample(bssidOnly)) == .available)
    // Granted but macOS returned neither: authorized but unavailable.
    #expect(contextAvailability(permission: .granted, read: .sample(neither)) == .authorizedUnavailable)
    #expect(contextAvailability(permission: .granted, read: .sample(emptySSID)) == .authorizedUnavailable)
    #expect(contextAvailability(permission: .granted, read: .noInterface) == .authorizedUnavailable)
    #expect(contextAvailability(permission: .granted, read: .failed) == .error)
    // Without permission the data is never claimed, even if a value slipped through.
    #expect(contextAvailability(permission: .notRequested, read: .sample(ssid)) == .notRequested)
    #expect(contextAvailability(permission: .denied, read: .sample(ssid)) == .denied)
    #expect(contextAvailability(permission: .restricted, read: .noInterface) == .restricted)
    #expect(contextAvailability(permission: .unsupported, read: .failed) == .unsupported)
    #expect(contextAvailability(permission: .error, read: .sample(ssid)) == .error)
}

@Test func theRequestActionNeedsTheDisclosureAndAnUnaskedPermission() {
    #expect(!canRequestLocationPermission(disclosureShown: false, status: .notRequested))
    #expect(canRequestLocationPermission(disclosureShown: true, status: .notRequested))
    for status in [PermissionStatus.granted, .denied, .restricted, .unsupported, .error] {
        #expect(!canRequestLocationPermission(disclosureShown: true, status: status))
    }
}
