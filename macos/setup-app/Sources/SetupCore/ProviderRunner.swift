// SPDX-License-Identifier: GPL-3.0-only

import Foundation

/// What the provider client reads from macOS. The app supplies the
/// CoreLocation/CoreWLAN implementation; tests supply a scripted one. Reading
/// must never prompt: only an explicit user action requests permission.
public protocol WifiSensor: AnyObject {
    func authorization() -> PermissionStatus
    func read() -> WifiReadResult
}

public enum ProviderConnection: Equatable, Sendable {
    case stopped
    case connecting
    case connected
}

/// Connects to the daemon's provider socket and keeps sending `hello`,
/// `permission_status` and `context` per docs/wifi-context-provider.md. It
/// retries about once per second while the daemon is absent, and it stops
/// sending the moment it is stopped, so the daemon reports the context as
/// unavailable within 15 s (a provider that is gone never looks alive).
public final class ProviderRunner {
    private let socketPath: String
    private let sensor: WifiSensor
    private let provider: String
    private let version: String
    private let pollMs: Int
    private let retryMs: Int
    private let lock = NSLock()
    private var stopRequested = false
    private var generation = 0
    private var connectionState = ProviderConnection.stopped
    private var thread: Thread?
    private let origin = DispatchTime.now().uptimeNanoseconds

    public init(
        socketPath: String = RunPaths.providerSocket(),
        sensor: WifiSensor,
        provider: String = "CYD Setup",
        version: String = "0.0.0",
        pollMs: Int = 250,
        retryMs: Int = 1000
    ) {
        self.socketPath = socketPath
        self.sensor = sensor
        self.provider = provider
        self.version = version
        self.pollMs = pollMs
        self.retryMs = retryMs
    }

    public var connection: ProviderConnection {
        lock.lock()
        defer { lock.unlock() }
        return connectionState
    }

    private func set(_ state: ProviderConnection) {
        lock.lock()
        connectionState = state
        lock.unlock()
    }

    /// True once `stop()` was called or a newer `start()` superseded this run.
    private func stopping(_ run: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return stopRequested || generation != run
    }

    public func start() {
        lock.lock()
        guard thread == nil else {
            lock.unlock()
            return
        }
        stopRequested = false
        generation += 1
        let run = generation
        let thread = Thread { [weak self] in self?.loop(run) }
        thread.name = "wifi-context-provider"
        self.thread = thread
        lock.unlock()
        thread.start()
    }

    public func stop() {
        lock.lock()
        stopRequested = true
        thread = nil
        lock.unlock()
    }

    private func monotonicMs() -> UInt64 {
        (DispatchTime.now().uptimeNanoseconds - origin) / 1_000_000
    }

    private func sleep(ms: Int, run: Int) {
        var left = ms
        while left > 0 && !stopping(run) {
            let slice = min(left, 50)
            Thread.sleep(forTimeInterval: Double(slice) / 1000)
            left -= slice
        }
    }

    private func loop(_ run: Int) {
        while !stopping(run) {
            set(.connecting)
            if let socket = try? UnixSocket.connect(path: socketPath) {
                try? serve(socket, run: run)
                socket.close()
            }
            set(.connecting)
            sleep(ms: retryMs, run: run)
        }
        set(.stopped)
    }

    /// One connection: hello, wait for the ack, then send until stopped or the
    /// daemon goes away (a write fails).
    func serve(_ socket: UnixSocket, run: Int) throws {
        var session = ProviderSession(provider: provider, version: version)
        try socket.writeLine(session.helloLine())
        let ack = try socket.readLine(maxBytes: ProviderMessageBuilder.maxLineBytes)
        guard ack.contains("\"hello_ack\"") else { throw SocketError.closed }
        set(.connected)
        var lastTMs: UInt64 = 0
        while !stopping(run) {
            let tMs = max(monotonicMs(), lastTMs)
            lastTMs = tMs
            for line in session.tick(tMs: tMs, permission: sensor.authorization(), read: sensor.read()) {
                try socket.writeLine(line)
            }
            sleep(ms: pollMs, run: run)
        }
    }
}
