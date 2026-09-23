// SPDX-License-Identifier: GPL-3.0-only

import Foundation

/// The LaunchAgent as `launchctl` sees it (never controlled from this app).
public struct ServiceState: Equatable, Sendable {
    public var installed: Bool
    public var loaded: Bool
    public var running: Bool
    public var pid: Int?

    public init(installed: Bool, loaded: Bool, running: Bool, pid: Int?) {
        self.installed = installed
        self.loaded = loaded
        self.running = running
        self.pid = pid
    }

    /// `launchctl print gui/<uid>/<label>`: exit status 0 means loaded; the
    /// top level (one tab of indent) carries `state = running` and `pid = N`.
    public static func parse(printOutput: String, exitCode: Int32, plistExists: Bool) -> ServiceState {
        guard exitCode == 0 else {
            return ServiceState(installed: plistExists, loaded: false, running: false, pid: nil)
        }
        var running = false
        var pid: Int?
        for line in printOutput.split(separator: "\n", omittingEmptySubsequences: false) {
            guard line.hasPrefix("\t"), !line.hasPrefix("\t\t") else { continue }
            let body = line.dropFirst()
            if body.hasPrefix("state = ") {
                running = body.dropFirst("state = ".count).trimmingCharacters(in: .whitespaces) == "running"
            } else if body.hasPrefix("pid = ") {
                pid = Int(body.dropFirst("pid = ".count).trimmingCharacters(in: .whitespaces))
            }
        }
        return ServiceState(installed: plistExists, loaded: true, running: running, pid: pid)
    }
}

/// Plain-language status for the setup window. No dashboard: states only.
public enum StatusText {
    public static func service(_ state: ServiceState?) -> String {
        guard let state else { return "Service: checking" }
        if state.running { return "Service: running" + (state.pid.map { " (pid \($0))" } ?? "") }
        if state.loaded { return "Service: loaded, not running" }
        if state.installed { return "Service: installed, not started (run `cyd-host service start`)" }
        return "Service: not installed (run `cyd-host service install`)"
    }

    public static func daemon(_ result: Result<DaemonStatus, ControlError>?) -> String {
        switch result {
        case nil: return "Host daemon: checking"
        case .success(let status): return "Host daemon: running, version \(status.daemon.version)"
        case .failure(.daemonNotRunning): return "Host daemon: not running"
        case .failure(.refused(_, let message)): return "Host daemon: refused the request (\(message))"
        case .failure: return "Host daemon: not answering as expected"
        }
    }

    public static func link(_ link: LinkInfo) -> String {
        let identity = link.pairingIdentity.map { " (display \($0))" } ?? ""
        switch link.state {
        case .noDevice: return "Display: none attached" + identity
        case .deviceFound: return "Display: found, connecting" + (link.detail.map { " (\($0))" } ?? "")
        case .pairingRequired:
            if let code = link.pairingCode {
                return "Display: pairing. Confirm code \(code) on the display."
            }
            return "Display: attached, not paired" + (link.detail.map { " (\($0))" } ?? "")
        case .pairedConnected: return "Display: paired and receiving data" + identity
        case .stale: return "Display: connection lost, retrying" + (link.detail.map { " (\($0))" } ?? "")
        case .unknown(let name): return "Display: \(name)"
        }
    }

    public static func permission(_ wifi: WifiContextInfo) -> String {
        let permission: String
        switch wifi.permission {
        case nil: permission = "not reported yet"
        case "not_requested": permission = "not requested"
        case "granted": permission = "granted"
        case "denied": permission = "denied"
        case "restricted": permission = "restricted by the system"
        case "unsupported": permission = "unsupported"
        default: permission = "error"
        }
        let context: String
        switch wifi.availability {
        case "available": context = "WiFi identity available"
        case "authorized_unavailable": context = "permission granted, but macOS returned no SSID/BSSID"
        case "not_requested": context = "no WiFi identity (optional)"
        case "denied", "restricted", "unsupported": context = "no WiFi identity (optional)"
        default: context = "WiFi context unavailable"
        }
        let provider: String
        switch wifi.provider {
        case "connected": provider = "provider connected"
        case "disconnected": provider = "provider disconnected"
        case "never_connected": provider = "no provider yet"
        default: provider = "provider socket unavailable"
        }
        return "Location permission: \(permission); \(context); \(provider). Monitoring does not depend on this."
    }

    /// One line per metric group, in a stable order.
    public static func metrics(_ metrics: [String: String]) -> [String] {
        let order = ["cpu", "ram", "storage", "battery", "agents", "network"]
        return order.compactMap { name in metrics[name].map { "\(name): \($0.replacingOccurrences(of: "_", with: " "))" } }
    }

    public static func drops(_ counters: Counters) -> String? {
        let p = counters.provider
        let control = counters.control
        let dropped =
            (p.map { $0.malformed + $0.oversized + $0.outOfOrder + $0.wrongUid + $0.rateLimited } ?? 0)
            + control.malformed + control.oversized + control.wrongUid
        return dropped > 0 ? "\(dropped) local IPC messages were dropped" : nil
    }
}
