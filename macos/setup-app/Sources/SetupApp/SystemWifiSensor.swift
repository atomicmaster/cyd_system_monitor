// SPDX-License-Identifier: GPL-3.0-only

import CoreLocation
import CoreWLAN
import Foundation
import SetupCore

/// The real CoreLocation/CoreWLAN reads. Creating it and reading the
/// authorization status never prompts; only `requestPermission()` does, and the
/// UI calls that solely from the disclosure-gated button.
final class SystemWifiSensor: NSObject, WifiSensor, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    private let lock = NSLock()
    private var cachedAuthorization = LocationAuthorization.notDetermined
    private var cachedServicesEnabled = true
    /// Called on the main queue when the authorization changes.
    var onChange: (() -> Void)?

    override init() {
        super.init()
        manager.delegate = self
        refresh()
    }

    private func refresh() {
        let mapped: LocationAuthorization
        switch manager.authorizationStatus {
        case .notDetermined: mapped = .notDetermined
        case .restricted: mapped = .restricted
        case .denied: mapped = .denied
        case .authorizedAlways: mapped = .authorizedAlways
        case .authorized: mapped = .authorizedWhenInUse
        @unknown default: mapped = .unknown
        }
        lock.lock()
        cachedAuthorization = mapped
        lock.unlock()
        // `locationServicesEnabled()` can block, so it is read off the main thread.
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let enabled = CLLocationManager.locationServicesEnabled()
            self?.lock.lock()
            self?.cachedServicesEnabled = enabled
            self?.lock.unlock()
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        refresh()
        DispatchQueue.main.async { [weak self] in self?.onChange?() }
    }

    func authorization() -> PermissionStatus {
        lock.lock()
        defer { lock.unlock() }
        return permissionStatus(for: cachedAuthorization, locationServicesEnabled: cachedServicesEnabled)
    }

    func read() -> WifiReadResult {
        guard let interface = CWWiFiClient.shared().interface() else { return .noInterface }
        let channel = interface.wlanChannel()
        let band: WifiBand?
        switch channel?.channelBand {
        case .band2GHz: band = .ghz2_4
        case .band5GHz: band = .ghz5
        case .band6GHz: band = .ghz6
        default: band = nil
        }
        let ssid = interface.ssidData().map { [UInt8]($0) }
        let bssid = interface.bssid()
        if channel == nil && ssid == nil && bssid == nil { return .noInterface }
        return .sample(
            WifiSample(band: band, channel: channel?.channelNumber, ssid: ssid, bssid: bssid))
    }

    /// The only place Location authorization is requested.
    func requestPermission() {
        manager.requestWhenInUseAuthorization()
    }
}
