// SPDX-License-Identifier: GPL-3.0-only

import Testing

@testable import SetupCore

@Test func helloAndPermissionLinesMatchTheContract() {
    var builder = ProviderMessageBuilder()
    #expect(
        builder.hello(provider: "CYD Setup", version: "0.0.0")
            == #"{"v":1,"type":"hello","provider":"CYD Setup","provider_version":"0.0.0"}"#)
    #expect(
        builder.permissionStatus(.granted)
            == #"{"v":1,"type":"permission_status","seq":0,"status":"granted"}"#)
}

@Test func contextLineCarriesAllNineFieldsAndHexSSID() {
    var builder = ProviderMessageBuilder()
    _ = builder.permissionStatus(.granted)
    let sample = WifiSample(band: .ghz5, channel: 36, ssid: [0x74, 0x65, 0x73, 0x74], bssid: "AA:BB:CC:DD:EE:FF")
    #expect(
        builder.context(tMs: 1500, availability: .available, sample: sample)
            == #"{"v":1,"type":"context","seq":1,"t_ms":1500,"age_ms":0,"availability":"available","band":"5","channel":36,"ssid_hex":"74657374","bssid":"aa:bb:cc:dd:ee:ff"}"#)
}

@Test func identityIsNeverSentUnlessAvailable() {
    var builder = ProviderMessageBuilder()
    let sample = WifiSample(band: .ghz2_4, channel: 6, ssid: [1, 2], bssid: "aa:bb:cc:dd:ee:ff")
    for availability in [ContextAvailability.authorizedUnavailable, .denied, .notRequested, .error] {
        let line = builder.context(tMs: 0, availability: availability, sample: sample)
        #expect(line.contains(#""ssid_hex":null,"bssid":null"#))
        // Band and channel need no permission and are still sent.
        #expect(line.contains(#""band":"2.4","channel":6"#))
    }
}

@Test func invalidValuesBecomeNullAndLinesStayWithinTheLimit() {
    var builder = ProviderMessageBuilder()
    let bad = WifiSample(band: nil, channel: 999, ssid: [UInt8](repeating: 1, count: 33), bssid: "not-a-mac")
    let line = builder.context(tMs: .max, availability: .available, sample: bad)
    #expect(line.contains(#""channel":null"#) && line.contains(#""ssid_hex":null"#) && line.contains(#""bssid":null"#))
    #expect(line.contains(#""band":null"#))
    #expect(line.utf8.count <= ProviderMessageBuilder.maxLineBytes)
    let maxSSID = WifiSample(band: .ghz6, channel: 233, ssid: [UInt8](repeating: 0xAB, count: 32), bssid: "aa:bb:cc:dd:ee:ff")
    #expect(builder.context(tMs: 1 << 53, availability: .available, sample: maxSSID).utf8.count <= 512)
    #expect(builder.hello(provider: "bad\"name\n", version: "").contains(#""provider":"bad_name_""#))
    #expect(builder.hello(provider: "", version: "").contains(#""provider":"unknown""#))
}

private func sequence(_ lines: [String]) -> [Int] {
    lines.compactMap { line in
        guard let range = line.range(of: #""seq":"#) else { return nil }
        return Int(line[range.upperBound...].prefix { $0.isNumber })
    }
}

@Test func aSessionSendsPermissionThenContextThenHeartbeatsEverySecond() {
    var session = ProviderSession(provider: "CYD Setup", version: "0.0.0")
    #expect(session.helloLine().contains(#""type":"hello""#))
    let read = WifiReadResult.sample(WifiSample(band: .ghz5, channel: 36))
    var all: [String] = []

    let first = session.tick(tMs: 0, permission: .notRequested, read: read)
    #expect(first.count == 2)
    #expect(first[0].contains(#""type":"permission_status""#) && first[0].contains(#""status":"not_requested""#))
    #expect(first[1].contains(#""type":"context""#) && first[1].contains(#""availability":"not_requested""#))
    all += first

    #expect(session.tick(tMs: 250, permission: .notRequested, read: read).isEmpty)
    #expect(session.tick(tMs: 999, permission: .notRequested, read: read).isEmpty)
    let heartbeat = session.tick(tMs: 1000, permission: .notRequested, read: read)
    #expect(heartbeat.count == 1 && heartbeat[0].contains(#""type":"context""#))
    all += heartbeat

    // A permission change goes out at once, with a context that reflects it.
    let changed = session.tick(tMs: 1100, permission: .granted, read: read)
    #expect(changed.count == 2)
    #expect(changed[0].contains(#""status":"granted""#))
    #expect(changed[1].contains(#""availability":"authorized_unavailable""#))
    all += changed

    // A change of association also goes out at once.
    let moved = session.tick(
        tMs: 1200, permission: .granted,
        read: .sample(WifiSample(band: .ghz2_4, channel: 6, ssid: [0x61], bssid: nil)))
    #expect(moved.count == 1 && moved[0].contains(#""availability":"available""#))
    all += moved

    let seqs = sequence(all)
    #expect(seqs == Array(0..<seqs.count), "seq strictly increasing across both message kinds")
}

@Test func aNewSessionRestartsSequenceNumbers() {
    var session = ProviderSession(provider: "CYD Setup", version: "0.0.0")
    _ = session.tick(tMs: 0, permission: .denied, read: .noInterface)
    var next = ProviderSession(provider: "CYD Setup", version: "0.0.0")
    let lines = next.tick(tMs: 0, permission: .denied, read: .noInterface)
    #expect(sequence(lines) == [0, 1])
}
