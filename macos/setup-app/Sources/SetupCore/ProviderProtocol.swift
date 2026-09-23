// SPDX-License-Identifier: GPL-3.0-only

import Foundation

/// Builds the provider-to-daemon JSON lines exactly as
/// docs/wifi-context-provider.md specifies: flat objects, `"v":1`, at most 512
/// bytes, `seq` strictly increasing across `permission_status` and `context`
/// within one connection, SSID/BSSID only with `available`.
public struct ProviderMessageBuilder {
    public static let maxLineBytes = 512
    private var seq: UInt64 = 0

    public init() {}

    public func hello(provider: String, version: String) -> String {
        line(
            "{\"v\":1,\"type\":\"hello\",\"provider\":\(Self.label(provider)),"
                + "\"provider_version\":\(Self.label(version))}")
    }

    public mutating func permissionStatus(_ status: PermissionStatus) -> String {
        defer { seq += 1 }
        return line(
            "{\"v\":1,\"type\":\"permission_status\",\"seq\":\(seq),\"status\":\"\(status.rawValue)\"}")
    }

    public mutating func context(
        tMs: UInt64,
        ageMs: UInt32 = 0,
        availability: ContextAvailability,
        sample: WifiSample?
    ) -> String {
        defer { seq += 1 }
        let identityAllowed = availability == .available
        let ssid = identityAllowed ? sample?.ssid.flatMap { $0.count <= 32 && !$0.isEmpty ? $0 : nil } : nil
        let bssid = identityAllowed ? sample?.bssid.flatMap(Self.validMac) : nil
        let channel = sample?.channel.flatMap { (1...233).contains($0) ? $0 : nil }
        return line(
            "{\"v\":1,\"type\":\"context\",\"seq\":\(seq),\"t_ms\":\(min(tMs, 1 << 53)),"
                + "\"age_ms\":\(ageMs),\"availability\":\"\(availability.rawValue)\","
                + "\"band\":\(sample?.band.map { "\"\($0.rawValue)\"" } ?? "null"),"
                + "\"channel\":\(channel.map(String.init) ?? "null"),"
                + "\"ssid_hex\":\(ssid.map { "\"" + $0.map { String(format: "%02x", $0) }.joined() + "\"" } ?? "null"),"
                + "\"bssid\":\(bssid.map { "\"\($0)\"" } ?? "null")}")
    }

    private func line(_ json: String) -> String {
        precondition(json.utf8.count <= Self.maxLineBytes, "provider line exceeds 512 bytes")
        return json
    }

    /// 1-32 of `A-Za-z0-9._- `; anything else is replaced.
    static func label(_ text: String) -> String {
        let allowed = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._- ")
        var clean = String(text.map { allowed.contains($0) ? $0 : "_" }.prefix(32))
        if clean.isEmpty { clean = "unknown" }
        return "\"\(clean)\""
    }

    static func validMac(_ text: String) -> String? {
        let parts = text.lowercased().split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 6,
            parts.allSatisfy({ $0.count == 2 && $0.allSatisfy(\.isHexDigit) })
        else { return nil }
        return parts.joined(separator: ":")
    }
}

/// One provider connection's send policy. A `context` goes out on every change
/// and at least once per second; `permission_status` goes out right after
/// `hello` and whenever the permission changes. A new session per connection
/// (`seq` and `t_ms` restart).
public struct ProviderSession {
    public static let heartbeatMs: UInt64 = 1_000

    private var builder = ProviderMessageBuilder()
    private var lastPermission: PermissionStatus?
    private var lastContext: (availability: ContextAvailability, read: WifiReadResult)?
    private var lastContextAt: UInt64?
    private let provider: String
    private let version: String

    public init(provider: String, version: String) {
        self.provider = provider
        self.version = version
    }

    public func helloLine() -> String {
        builder.hello(provider: provider, version: version)
    }

    /// The lines to send for the current observation (possibly none).
    public mutating func tick(
        tMs: UInt64,
        permission: PermissionStatus,
        read: WifiReadResult
    ) -> [String] {
        var lines: [String] = []
        let permissionChanged = permission != lastPermission
        if permissionChanged {
            lines.append(builder.permissionStatus(permission))
            lastPermission = permission
        }
        let availability = contextAvailability(permission: permission, read: read)
        let changed =
            lastContext.map { $0.availability != availability || $0.read != read } ?? true
        let due = lastContextAt.map { tMs >= $0 + Self.heartbeatMs } ?? true
        if permissionChanged || changed || due {
            var sample: WifiSample?
            if case .sample(let s) = read { sample = s }
            lines.append(builder.context(tMs: tMs, availability: availability, sample: sample))
            lastContext = (availability, read)
            lastContextAt = tMs
        }
        return lines
    }
}
