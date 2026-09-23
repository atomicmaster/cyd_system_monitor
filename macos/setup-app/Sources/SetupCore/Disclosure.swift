// SPDX-License-Identifier: GPL-3.0-only

/// The WiFi identity disclosure, verbatim from docs/macos-components.md. It is
/// shown before any Location permission request; a test keeps the two equal.
public enum Disclosure {
    public static let text =
        "macOS requires Location Services to reveal the connected WiFi SSID/BSSID. "
        + "This improves correlation of wireless Alerts. CYD System Monitor does not request "
        + "coordinates, track movement, or store location. You may skip this; monitoring "
        + "continues with reduced association context."
}
