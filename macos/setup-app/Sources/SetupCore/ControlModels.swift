// SPDX-License-Identifier: GPL-3.0-only

import Foundation

// Models for the daemon's control IPC, version 1 (docs/host-control-ipc.md).
// Decoded with `.convertFromSnakeCase`; unknown keys are ignored so the schema
// can grow additively.

public enum LinkState: Equatable, Sendable {
    case noDevice, deviceFound, pairingRequired, pairedConnected, stale
    /// A state a newer daemon reports that this build does not know.
    case unknown(String)

    init(wire: String) {
        switch wire {
        case "no_device": self = .noDevice
        case "device_found": self = .deviceFound
        case "pairing_required": self = .pairingRequired
        case "paired_connected": self = .pairedConnected
        case "stale": self = .stale
        default: self = .unknown(wire)
        }
    }
}

public struct DaemonInfo: Decodable, Equatable, Sendable {
    public var version: String
    public var pid: Int
    public var uptimeS: Int
    public var uid: Int
}

public struct LinkInfo: Decodable, Equatable, Sendable {
    public var stateName: String
    public var detail: String?
    public var port: String?
    public var pairingIdentity: String?
    public var pairingCode: String?
    public var sessions: Int
    public var snapshots: Int
    public var lastSnapshotUtcMs: Int64?
    public var lastSnapshotAgeMs: Int64?

    public var state: LinkState { LinkState(wire: stateName) }

    enum CodingKeys: String, CodingKey {
        case stateName = "state"
        case detail, port, pairingIdentity, pairingCode, sessions, snapshots
        case lastSnapshotUtcMs, lastSnapshotAgeMs
    }
}

public struct WifiContextInfo: Decodable, Equatable, Sendable {
    /// `never_connected`, `connected`, `disconnected` or `server_unavailable`.
    public var provider: String
    public var availability: String
    /// The Location permission as the provider last reported it.
    public var permission: String?
}

public struct ProviderCounters: Decodable, Equatable, Sendable {
    public var accepted, replaced, wrongUid, malformed, oversized: Int
    public var outOfOrder, rateLimited, badVersion, closedForBadLines, applied: Int
}

public struct ControlCounters: Decodable, Equatable, Sendable {
    public var accepted, requests, wrongUid, malformed, oversized: Int
    public var badVersion, unknownCommand, busy, closedForBadLines: Int
}

public struct Counters: Decodable, Equatable, Sendable {
    public var provider: ProviderCounters?
    public var control: ControlCounters
}

public struct DaemonStatus: Decodable, Equatable, Sendable {
    public var daemon: DaemonInfo
    public var link: LinkInfo
    /// Metric group name to availability; empty unless paired and connected.
    public var metrics: [String: String]
    public var wifiContext: WifiContextInfo
    public var counters: Counters
}

/// The envelope of every response.
public struct ControlReply<Body: Decodable>: Decodable {
    public var v: Int
    public var ok: Bool
    public var cmd: String?
    public var error: String?
    public var message: String?
    public var status: Body?
}

public struct MonitorInfo: Decodable, Equatable, Sendable {
    public var port: String
    public var pairingIdentity: String?
    public var state: String
}

public struct MonitorsReply: Decodable {
    public var ok: Bool
    public var monitors: [MonitorInfo]?
}

public enum ControlError: Error, Equatable {
    case daemonNotRunning
    case transport(SocketError)
    case badReply
    case refused(code: String, message: String)
}

/// A small synchronous client of the daemon's control socket.
public struct ControlClient {
    public static let maxReplyBytes = 8192
    public var socketPath: String

    public init(socketPath: String = RunPaths.controlSocket()) {
        self.socketPath = socketPath
    }

    static func decoder() -> JSONDecoder {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return decoder
    }

    func roundTrip(_ line: String) throws -> Data {
        do {
            let socket = try UnixSocket.connect(path: socketPath)
            try socket.writeLine(line)
            return Data(try socket.readLine(maxBytes: Self.maxReplyBytes).utf8)
        } catch SocketError.notRunning {
            throw ControlError.daemonNotRunning
        } catch let error as SocketError {
            throw ControlError.transport(error)
        }
    }

    public static func requestLine(_ command: String, port: String? = nil) -> String {
        if let port { return "{\"v\":1,\"cmd\":\"\(command)\",\"port\":\"\(port)\"}" }
        return "{\"v\":1,\"cmd\":\"\(command)\"}"
    }

    public static func parseStatus(_ data: Data) throws -> DaemonStatus {
        guard let reply = try? decoder().decode(ControlReply<DaemonStatus>.self, from: data), reply.v == 1
        else { throw ControlError.badReply }
        guard reply.ok, let status = reply.status else {
            throw ControlError.refused(code: reply.error ?? "unknown", message: reply.message ?? "")
        }
        return status
    }

    public func status() throws -> DaemonStatus {
        try Self.parseStatus(roundTrip(Self.requestLine("status")))
    }

    /// `pair_start`, `pair_cancel`, `unpair` and the like: succeed or throw.
    public func command(_ name: String, port: String? = nil) throws {
        let data = try roundTrip(Self.requestLine(name, port: port))
        guard let reply = try? Self.decoder().decode(ControlReply<DaemonStatus>.self, from: data)
        else { throw ControlError.badReply }
        if !reply.ok {
            throw ControlError.refused(code: reply.error ?? "unknown", message: reply.message ?? "")
        }
    }
}
