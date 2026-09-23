// SPDX-License-Identifier: GPL-3.0-only

import Foundation
import Testing

@testable import SetupCore

/// A throwaway same-user Unix socket server in a temp directory (never a real
/// user directory). `handler` runs once per accepted connection.
final class FakeServer {
    let directory: String
    let path: String
    private var listener: Int32 = -1
    private var running = true
    private var thread: Thread?

    init(name: String, directoryMode: mode_t = 0o700, socketMode: mode_t = 0o600,
         handler: @escaping (Int32) -> Void) throws {
        signal(SIGPIPE, SIG_IGN)  // the test server may write to a client that hung up
        directory = "/tmp/cyd-swift-\(name)-\(getpid())"
        path = directory + "/s.sock"
        try? FileManager.default.removeItem(atPath: directory)
        try FileManager.default.createDirectory(
            atPath: directory, withIntermediateDirectories: true,
            attributes: [.posixPermissions: NSNumber(value: directoryMode)])
        // createDirectory is subject to umask; enforce the mode under test.
        chmod(directory, directoryMode)
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        withUnsafeMutablePointer(to: &address.sun_path) {
            $0.withMemoryRebound(to: UInt8.self, capacity: 104) { p in
                for (i, b) in bytes.enumerated() { p[i] = b }
                p[bytes.count] = 0
            }
        }
        listener = socket(AF_UNIX, SOCK_STREAM, 0)
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(listener, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        precondition(bound == 0 && listen(listener, 4) == 0)
        chmod(path, socketMode)
        let fd = listener
        let thread = Thread { [weak self] in
            while self?.running ?? false {
                var pollFd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                if poll(&pollFd, 1, 50) > 0 {
                    let client = accept(fd, nil, nil)
                    if client >= 0 {
                        handler(client)
                        close(client)
                    }
                }
            }
        }
        self.thread = thread
        thread.start()
    }

    deinit {
        running = false
        Thread.sleep(forTimeInterval: 0.1)
        close(listener)
        try? FileManager.default.removeItem(atPath: directory)
    }
}

func readLine(_ fd: Int32) -> String {
    var out: [UInt8] = []
    var byte: UInt8 = 0
    while read(fd, &byte, 1) == 1 && byte != 0x0A { out.append(byte) }
    return String(decoding: out, as: UTF8.self)
}

func send(_ fd: Int32, _ text: String) {
    let bytes = Array((text + "\n").utf8)
    _ = bytes.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
}

@Test func aMissingSocketMeansTheDaemonIsNotRunning() {
    let client = ControlClient(socketPath: "/tmp/cyd-swift-absent-\(getpid())/control.sock")
    #expect(throws: ControlError.daemonNotRunning) { try client.status() }
}

@Test func statusRoundTripsOverASameUserSocket() throws {
    let reply = try String(contentsOf: URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().appendingPathComponent("Fixtures/status-paired.json"), encoding: .utf8)
        .trimmingCharacters(in: .whitespacesAndNewlines)
    let requests = Locked<[String]>([])
    let server = try FakeServer(name: "status") { fd in
        requests.mutate { $0.append(readLine(fd)) }
        send(fd, reply)
    }
    let status = try ControlClient(socketPath: server.path).status()
    #expect(status.link.state == .pairedConnected)
    #expect(requests.value == [#"{"v":1,"cmd":"status"}"#])
}

@Test func anOversizedReplyIsRefusedNotBuffered() throws {
    let server = try FakeServer(name: "big") { fd in
        _ = readLine(fd)
        send(fd, String(repeating: "x", count: 100_000))
    }
    #expect(throws: ControlError.transport(.oversized)) { try ControlClient(socketPath: server.path).status() }
}

@Test func aSilentServerTimesOutInsteadOfHanging() throws {
    let server = try FakeServer(name: "silent") { fd in
        _ = readLine(fd)
        Thread.sleep(forTimeInterval: 3)
    }
    let socket = try UnixSocket.connect(path: server.path, timeoutSeconds: 1)
    try socket.writeLine("{}")
    #expect(throws: SocketError.timeout) { _ = try socket.readLine(maxBytes: 100) }
}

@Test func aSocketOrDirectoryOthersCouldReachIsRefused() throws {
    let openDirectory = try FakeServer(name: "opendir", directoryMode: 0o755) { _ in }
    #expect(throws: SocketError.unsafePath("directory")) { _ = try UnixSocket.connect(path: openDirectory.path) }
    let openSocket = try FakeServer(name: "opensock", socketMode: 0o666) { _ in }
    #expect(throws: SocketError.unsafePath("socket")) { _ = try UnixSocket.connect(path: openSocket.path) }
}

@Test func commandsSurfaceTheDaemonsRefusal() throws {
    let server = try FakeServer(name: "refuse") { fd in
        _ = readLine(fd)
        send(fd, #"{"v":1,"ok":false,"error":"already_paired","message":"a display is already paired; unpair it first"}"#)
    }
    #expect(throws: ControlError.refused(code: "already_paired", message: "a display is already paired; unpair it first")) {
        try ControlClient(socketPath: server.path).command("pair_start")
    }
}

final class Locked<T> {
    private let lock = NSLock()
    private var stored: T
    init(_ value: T) { stored = value }
    var value: T {
        lock.lock()
        defer { lock.unlock() }
        return stored
    }
    func mutate(_ change: (inout T) -> Void) {
        lock.lock()
        change(&stored)
        lock.unlock()
    }
}
