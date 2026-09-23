// SPDX-License-Identifier: GPL-3.0-only

import Foundation

public enum SocketError: Error, Equatable {
    /// No socket, or nothing listening: the daemon is simply not running.
    case notRunning
    /// The directory or socket is not private to this user.
    case unsafePath(String)
    case pathTooLong
    /// A line longer than the limit; the connection is unusable.
    case oversized
    case timeout
    case closed
    /// The peer is not this user's process.
    case wrongPeer
    case io(Int32)
}

/// The per-user run directory shared with the daemon: `$CYD_RUN_DIR`, else
/// `~/Library/Application Support/CYD System Monitor/run`.
public enum RunPaths {
    public static func runDirectory(environment: [String: String] = ProcessInfo.processInfo.environment) -> String {
        if let dir = environment["CYD_RUN_DIR"], !dir.isEmpty { return dir }
        return NSHomeDirectory() + "/Library/Application Support/CYD System Monitor/run"
    }

    public static func controlSocket(runDirectory: String = runDirectory()) -> String {
        runDirectory + "/control.sock"
    }

    public static func providerSocket(runDirectory: String = runDirectory()) -> String {
        runDirectory + "/wifi-context.sock"
    }
}

/// A connected, same-user Unix stream socket with bounded, timed line reads.
public final class UnixSocket {
    private var fd: Int32
    private var pending: [UInt8] = []

    private init(fd: Int32) { self.fd = fd }

    deinit { close() }

    /// Refuses a directory or socket that another user could reach: the
    /// directory must be a real directory owned by this user with no group or
    /// other access, and the socket a socket owned by this user with mode 0600.
    public static func verifyPrivate(path: String) throws {
        let directory = (path as NSString).deletingLastPathComponent
        var info = stat()
        guard lstat(directory, &info) == 0 else { throw SocketError.notRunning }
        guard (info.st_mode & S_IFMT) == S_IFDIR, info.st_uid == getuid(), info.st_mode & 0o077 == 0 else {
            throw SocketError.unsafePath("directory")
        }
        guard lstat(path, &info) == 0 else { throw SocketError.notRunning }
        guard (info.st_mode & S_IFMT) == S_IFSOCK, info.st_uid == getuid(), info.st_mode & 0o077 == 0 else {
            throw SocketError.unsafePath("socket")
        }
    }

    public static func connect(path: String, timeoutSeconds: Int = 2) throws -> UnixSocket {
        try verifyPrivate(path: path)
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let capacity = MemoryLayout.size(ofValue: address.sun_path)
        let bytes = Array(path.utf8)
        guard bytes.count < capacity else { throw SocketError.pathTooLong }
        withUnsafeMutablePointer(to: &address.sun_path) {
            $0.withMemoryRebound(to: UInt8.self, capacity: capacity) { destination in
                for (index, byte) in bytes.enumerated() { destination[index] = byte }
                destination[bytes.count] = 0
            }
        }
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw SocketError.io(errno) }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var timeout = timeval(tv_sec: timeoutSeconds, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else {
            let error = errno
            Darwin.close(fd)
            throw (error == ENOENT || error == ECONNREFUSED) ? SocketError.notRunning : SocketError.io(error)
        }
        var uid: uid_t = 0
        var gid: gid_t = 0
        guard getpeereid(fd, &uid, &gid) == 0, uid == getuid() else {
            Darwin.close(fd)
            throw SocketError.wrongPeer
        }
        return UnixSocket(fd: fd)
    }

    public func writeLine(_ text: String) throws {
        var data = Array(text.utf8)
        data.append(0x0A)
        var sent = 0
        while sent < data.count {
            let n = data[sent...].withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
            if n < 0 {
                if errno == EINTR { continue }
                throw errno == EPIPE || errno == ECONNRESET ? SocketError.closed : SocketError.io(errno)
            }
            sent += n
        }
    }

    /// One line without its newline. A line longer than `maxBytes` throws
    /// `oversized` without buffering more than `maxBytes + one chunk`.
    public func readLine(maxBytes: Int) throws -> String {
        var chunk = [UInt8](repeating: 0, count: 512)
        while true {
            if let newline = pending.firstIndex(of: 0x0A) {
                let line = Array(pending[..<newline])
                pending.removeSubrange(...newline)
                guard line.count <= maxBytes else { throw SocketError.oversized }
                return String(decoding: line, as: UTF8.self)
            }
            if pending.count > maxBytes { throw SocketError.oversized }
            let n = Darwin.read(fd, &chunk, chunk.count)
            if n == 0 { throw SocketError.closed }
            if n < 0 {
                if errno == EINTR { continue }
                if errno == EAGAIN || errno == EWOULDBLOCK { throw SocketError.timeout }
                throw SocketError.io(errno)
            }
            pending.append(contentsOf: chunk[..<n])
        }
    }

    public func close() {
        if fd >= 0 {
            Darwin.close(fd)
            fd = -1
        }
    }
}
