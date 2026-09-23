// SPDX-License-Identifier: GPL-3.0-only

import Foundation
import Testing

@testable import SetupCore

private final class ScriptedSensor: WifiSensor {
    private let lock = NSLock()
    private var permission = PermissionStatus.notRequested
    func set(_ value: PermissionStatus) {
        lock.lock()
        permission = value
        lock.unlock()
    }
    func authorization() -> PermissionStatus {
        lock.lock()
        defer { lock.unlock() }
        return permission
    }
    func read() -> WifiReadResult { .sample(WifiSample(band: .ghz5, channel: 36)) }
}

private func waitFor(_ what: String, seconds: Double = 5, _ condition: () -> Bool) -> Bool {
    let end = Date().addingTimeInterval(seconds)
    while Date() < end {
        if condition() { return true }
        Thread.sleep(forTimeInterval: 0.02)
    }
    return false
}

@Test func theRunnerSpeaksHelloPermissionThenContextAndReactsToPermissionChanges() throws {
    let received = Locked<[String]>([])
    let server = try FakeServer(name: "prov") { fd in
        received.mutate { $0.append(readLine(fd)) }  // hello
        send(fd, #"{"v":1,"type":"hello_ack","heartbeat_max_ms":1000,"silence_limit_ms":15000}"#)
        while true {
            let line = readLine(fd)
            if line.isEmpty { return }  // the runner hung up
            received.mutate { $0.append(line) }
        }
    }
    let sensor = ScriptedSensor()
    let runner = ProviderRunner(socketPath: server.path, sensor: sensor, pollMs: 20, retryMs: 50)
    runner.start()
    defer { runner.stop() }

    #expect(waitFor("three lines") { received.value.count >= 3 })
    let head = received.value
    #expect(head[0].contains(#""type":"hello""#))
    #expect(head[1].contains(#""type":"permission_status""#) && head[1].contains("not_requested"))
    #expect(head[2].contains(#""type":"context""#) && head[2].contains(#""availability":"not_requested""#))
    #expect(runner.connection == .connected)

    // A change of permission is reported without waiting for the heartbeat.
    let before = received.value.count
    sensor.set(.denied)
    #expect(waitFor("denied") { received.value.dropFirst(before).contains { $0.contains(#""status":"denied""#) } })
}

@Test func theRunnerKeepsRetryingWithoutADaemonAndStopsCleanly() throws {
    let runner = ProviderRunner(
        socketPath: "/tmp/cyd-swift-none-\(getpid())/wifi-context.sock",
        sensor: ScriptedSensor(), pollMs: 20, retryMs: 20)
    runner.start()
    #expect(waitFor("connecting") { runner.connection == .connecting })
    Thread.sleep(forTimeInterval: 0.2)
    #expect(runner.connection == .connecting, "an absent daemon is not an error state")
    runner.stop()
    #expect(waitFor("stopped") { runner.connection == .stopped })
}

@Test func aDaemonThatWithholdsTheAckGetsNoData() throws {
    let received = Locked<[String]>([])
    let server = try FakeServer(name: "noack") { fd in
        received.mutate { $0.append(readLine(fd)) }
        send(fd, #"{"v":1,"type":"error","reason":"unsupported_version"}"#)
    }
    let runner = ProviderRunner(socketPath: server.path, sensor: ScriptedSensor(), pollMs: 20, retryMs: 50)
    runner.start()
    defer { runner.stop() }
    #expect(waitFor("a hello") { !received.value.isEmpty })
    Thread.sleep(forTimeInterval: 0.2)
    #expect(received.value.allSatisfy { $0.contains(#""type":"hello""#) })
}
