// SPDX-License-Identifier: GPL-3.0-only

/// CoreLocation's authorization, without importing CoreLocation.
public enum LocationAuthorization: Equatable, Sendable {
    case notDetermined
    case restricted
    case denied
    case authorizedAlways
    case authorizedWhenInUse
    /// A value this build does not know: reported as `error`, never guessed.
    case unknown
}

/// The `permission_status` wire values (docs/wifi-context-provider.md).
public enum PermissionStatus: String, Equatable, Sendable {
    case notRequested = "not_requested"
    case denied
    case restricted
    case granted
    case unsupported
    case error
}

/// The shared Association Context Availability wire values.
public enum ContextAvailability: String, Equatable, Sendable {
    case available
    case notRequested = "not_requested"
    case denied
    case restricted
    case authorizedUnavailable = "authorized_unavailable"
    case unsupported
    case error
}

public enum WifiBand: String, Equatable, Sendable {
    case ghz2_4 = "2.4"
    case ghz5 = "5"
    case ghz6 = "6"
}

/// What CoreWLAN returned for the current association.
public struct WifiSample: Equatable, Sendable {
    public var band: WifiBand?
    public var channel: Int?
    public var ssid: [UInt8]?
    public var bssid: String?

    public init(band: WifiBand? = nil, channel: Int? = nil, ssid: [UInt8]? = nil, bssid: String? = nil) {
        self.band = band
        self.channel = channel
        self.ssid = ssid
        self.bssid = bssid
    }
}

/// The SSID and BSSID of a sample as they may go on the wire: an SSID of 1-32
/// octets, a BSSID of six two-digit hex groups (normalised to lowercase).
/// Anything else is dropped. The availability mapping and the message builder
/// both use this, so `available` is claimed exactly when one of them is sent.
public struct WifiIdentity: Equatable, Sendable {
    public var ssid: [UInt8]?
    public var bssid: String?

    public init(ssid: [UInt8]?, bssid: String?) {
        self.ssid = ssid
        self.bssid = bssid
    }

    public var isEmpty: Bool { ssid == nil && bssid == nil }
}

public func sanitizedIdentity(_ sample: WifiSample) -> WifiIdentity {
    let ssid = sample.ssid.flatMap { (1...32).contains($0.count) ? $0 : nil }
    let bssid = sample.bssid.flatMap { text -> String? in
        let parts = text.lowercased().split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 6,
            parts.allSatisfy({ $0.count == 2 && $0.allSatisfy(\.isHexDigit) })
        else { return nil }
        return parts.joined(separator: ":")
    }
    return WifiIdentity(ssid: ssid, bssid: bssid)
}

public enum WifiReadResult: Equatable, Sendable {
    /// This Mac has no WiFi interface (or it is not associated to read).
    case noInterface
    /// CoreWLAN failed.
    case failed
    case sample(WifiSample)
}

/// Location authorization to the provider's `permission_status`.
/// `granted` says nothing about whether SSID/BSSID are readable.
public func permissionStatus(
    for authorization: LocationAuthorization,
    locationServicesEnabled: Bool
) -> PermissionStatus {
    switch authorization {
    case .notDetermined: return .notRequested
    case .restricted: return .restricted
    case .denied: return .denied
    case .authorizedAlways, .authorizedWhenInUse:
        // System-wide Location Services off means nothing can be revealed.
        return locationServicesEnabled ? .granted : .denied
    case .unknown: return .error
    }
}

/// The Association Context Availability for a permission and a CoreWLAN read:
/// `available` only when a valid SSID or BSSID came back (one that
/// [`sanitizedIdentity`] keeps and the builder will send); a granted permission
/// that yields neither is `authorized_unavailable`; every other permission
/// state maps to itself.
public func contextAvailability(permission: PermissionStatus, read: WifiReadResult) -> ContextAvailability {
    switch permission {
    case .notRequested: return .notRequested
    case .denied: return .denied
    case .restricted: return .restricted
    case .unsupported: return .unsupported
    case .error: return .error
    case .granted:
        switch read {
        case .failed: return .error
        case .noInterface: return .authorizedUnavailable
        case .sample(let sample):
            return sanitizedIdentity(sample).isEmpty ? .authorizedUnavailable : .available
        }
    }
}

/// Whether the "Request Location permission" action may run: only once the
/// disclosure has been shown, and only while nothing was asked yet.
public func canRequestLocationPermission(disclosureShown: Bool, status: PermissionStatus) -> Bool {
    disclosureShown && status == .notRequested
}
