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
    #expect(session.tick(tMs: 899, permission: .notRequested, read: read).isEmpty)
    let heartbeat = session.tick(tMs: 900, permission: .notRequested, read: read)
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

@Test func theHeartbeatIsDueBeforeTheNextLookCouldMakeItLate() {
    let schedule = HeartbeatSchedule(heartbeatMs: 1_000, pollMs: 250, leadMs: 100)
    #expect(schedule.due(nowMs: 0, lastSentMs: nil))
    #expect(!schedule.due(nowMs: 899, lastSentMs: 0))
    #expect(schedule.due(nowMs: 900, lastSentMs: 0))
    // The wait is the poll interval, cut short so the look lands on time.
    #expect(schedule.waitMs(nowMs: 0, lastSentMs: nil) == 250)
    #expect(schedule.waitMs(nowMs: 100, lastSentMs: 0) == 250)
    #expect(schedule.waitMs(nowMs: 750, lastSentMs: 0) == 150)
    #expect(schedule.waitMs(nowMs: 950, lastSentMs: 0) == 0)
    // A clock that went backwards (never with a monotonic clock) is due.
    #expect(schedule.due(nowMs: 0, lastSentMs: 5_000))
}

@Test func heartbeatsNeverMoreThanOneSecondApartDespiteSlowTicksAndLateWakeups() {
    // A polling loop whose every tick (sensor read plus send) and every sleep
    // overrun take up to 99 ms in total: the gap between two context lines
    // stays within one second, where a plain 250 ms poll let it reach 1.25 s.
    let schedule = HeartbeatSchedule(heartbeatMs: 1_000, pollMs: 250, leadMs: 100)
    var state: UInt64 = 0x9E37_79B9_7F4A_7C15
    func jitter(_ bound: UInt64) -> UInt64 {
        state ^= state << 13
        state ^= state >> 7
        state ^= state << 17
        return state % bound
    }
    var now: UInt64 = 0
    var last: UInt64?
    var sends: [UInt64] = []
    while now < 600_000 {
        if schedule.due(nowMs: now, lastSentMs: last) {
            sends.append(now)
            last = now
        }
        let overrun = jitter(100)
        now += overrun / 2  // the tick itself
        now += schedule.waitMs(nowMs: now, lastSentMs: last) + (overrun - overrun / 2)
    }
    let gaps = zip(sends.dropFirst(), sends).map { $0 - $1 }
    #expect(gaps.max()! <= 1_000)
    #expect(gaps.min()! >= 800, "not much more often than needed")
}

@Test func aSessionSendsTheHeartbeatAheadOfTheSecond() {
    var session = ProviderSession(provider: "CYD Setup", version: "0.0.0", pollMs: 250)
    let read = WifiReadResult.sample(WifiSample(band: .ghz5, channel: 36))
    #expect(session.tick(tMs: 0, permission: .denied, read: read).count == 2)
    #expect(session.tick(tMs: 750, permission: .denied, read: read).isEmpty)
    #expect(session.nextWaitMs(tMs: 750) == 150)
    #expect(session.tick(tMs: 900, permission: .denied, read: read).count == 1)
}
