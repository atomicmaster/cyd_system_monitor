// SPDX-License-Identifier: GPL-3.0-only

import Foundation
import Testing

@testable import SetupCore

private func fixture(_ name: String) throws -> Data {
    let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        .appendingPathComponent("Fixtures/\(name).json")
    return try Data(contentsOf: url)
}

@Test func decodesTheDaemonsNoDeviceStatus() throws {
    // Captured from a real `cyd-hostd` run against a temp directory.
    let status = try ControlClient.parseStatus(fixture("status-no-device"))
    #expect(status.daemon.pid == 66556 && status.daemon.uid == 501)
    #expect(status.link.state == .noDevice)
    #expect(status.link.pairingIdentity == nil && status.link.lastSnapshotUtcMs == nil)
    #expect(status.metrics.isEmpty)
    #expect(status.wifiContext == WifiContextInfo(provider: "never_connected", availability: "not_requested", permission: nil))
    #expect(status.counters.provider?.malformed == 0 && status.counters.control.requests == 1)
}

@Test func decodesAPairedStatusWithGroupsAndDrops() throws {
    let status = try ControlClient.parseStatus(fixture("status-paired"))
    #expect(status.link.state == .pairedConnected)
    #expect(status.link.pairingIdentity == "d0d0d0d0" && status.link.sessions == 2)
    #expect(status.link.lastSnapshotAgeMs == 412)
    #expect(status.metrics["battery"] == "absent")
    #expect(status.wifiContext.permission == "granted")
    #expect(StatusText.drops(status.counters) == "3 local IPC messages were dropped")
}

@Test func unknownKeysAndStatesAreTolerated() throws {
    let text = #"{"v":1,"ok":true,"cmd":"status","extra":1,"status":{"daemon":{"version":"9","pid":1,"uptime_s":1,"uid":1,"new":true},"link":{"state":"hibernating","detail":null,"port":null,"pairing_identity":null,"pairing_code":null,"sessions":0,"snapshots":0,"last_snapshot_utc_ms":null,"last_snapshot_age_ms":null},"metrics":{},"wifi_context":{"provider":"connected","availability":"error","permission":null},"counters":{"provider":null,"control":{"accepted":0,"requests":0,"wrong_uid":0,"malformed":0,"oversized":0,"bad_version":0,"unknown_command":0,"busy":0,"closed_for_bad_lines":0}}}}"#
    let status = try ControlClient.parseStatus(Data(text.utf8))
    #expect(status.link.state == .unknown("hibernating"))
    #expect(status.counters.provider == nil)
}

@Test func refusalsAndGarbageAreErrorsNotCrashes() {
    let refusal = #"{"v":1,"ok":false,"error":"not_connected","message":"no paired display is connected"}"#
    #expect(throws: ControlError.refused(code: "not_connected", message: "no paired display is connected")) {
        try ControlClient.parseStatus(Data(refusal.utf8))
    }
    for garbage in ["", "nope", "{}", #"{"v":2,"ok":true}"#, "[1]"] {
        #expect(throws: ControlError.badReply) { try ControlClient.parseStatus(Data(garbage.utf8)) }
    }
}

@Test func requestLinesAreTheDocumentedShape() {
    #expect(ControlClient.requestLine("status") == #"{"v":1,"cmd":"status"}"#)
    #expect(ControlClient.requestLine("pair_start", port: "/dev/cu.x") == #"{"v":1,"cmd":"pair_start","port":"/dev/cu.x"}"#)
}

@Test func statusTextIsPlainAndStateAware() throws {
    let no = try ControlClient.parseStatus(fixture("status-no-device"))
    let paired = try ControlClient.parseStatus(fixture("status-paired"))
    #expect(StatusText.daemon(.success(no)) == "Host daemon: running, version 0.0.0")
    #expect(StatusText.daemon(.failure(.daemonNotRunning)) == "Host daemon: not running")
    #expect(StatusText.link(no.link) == "Display: none attached")
    #expect(StatusText.link(paired.link) == "Display: paired and receiving data (display d0d0d0d0)")
    var pairing = paired.link
    pairing.stateName = "pairing_required"
    pairing.pairingCode = "123456"
    #expect(StatusText.link(pairing) == "Display: pairing. Confirm code 123456 on the display.")
    #expect(StatusText.permission(paired.wifiContext).contains("permission granted, but macOS returned no SSID/BSSID"))
    #expect(StatusText.permission(no.wifiContext).contains("no provider yet"))
    // The glossary term (CONTEXT.md), never the ambiguous "WiFi identity".
    #expect(StatusText.permission(no.wifiContext).contains("no Association Context (optional)"))
    #expect(!StatusText.permission(paired.wifiContext).contains("WiFi identity"))
    #expect(StatusText.permission(no.wifiContext).hasSuffix("Monitoring does not depend on this."))
    #expect(StatusText.metrics(paired.metrics).first == "cpu: available")
    #expect(StatusText.drops(no.counters) == nil)
}

private let running = "gui/501/com.cydsystemmonitor.hostd = {\n\tactive count = 1\n\ttype = LaunchAgent\n\tstate = running\n\n\targuments = {\n\t\t/opt/cyd/bin/cyd-hostd\n\t}\n\n\tpid = 4242\n}\n"
private let waiting = "gui/501/com.cydsystemmonitor.hostd = {\n\tstate = not running\n\n\tlast exit code = 1\n}\n"

@Test func serviceStateComesFromLaunchctlPrint() {
    let up = ServiceState.parse(printOutput: running, exitCode: 0, plistExists: true)
    #expect(up == ServiceState(installed: true, loaded: true, running: true, pid: 4242))
    #expect(StatusText.service(up) == "Service: running (pid 4242)")
    let idle = ServiceState.parse(printOutput: waiting, exitCode: 0, plistExists: true)
    #expect(idle == ServiceState(installed: true, loaded: true, running: false, pid: nil))
    #expect(StatusText.service(idle) == "Service: loaded, not running")
    let stopped = ServiceState.parse(printOutput: "", exitCode: 113, plistExists: true)
    #expect(StatusText.service(stopped).hasPrefix("Service: installed, not started"))
    let none = ServiceState.parse(printOutput: "", exitCode: 113, plistExists: false)
    #expect(StatusText.service(none).hasPrefix("Service: not installed"))
    #expect(StatusText.service(nil) == "Service: checking")
}
