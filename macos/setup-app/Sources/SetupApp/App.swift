// SPDX-License-Identifier: GPL-3.0-only
//
// Development-build SwiftUI status/setup app: no dashboard. It shows the
// daemon's status, the LaunchAgent state, pairing state and the optional WiFi
// permission state, and (behind the disclosure) offers the Location permission
// request. Nothing is requested at launch. The daemon does not depend on this
// app; closing it stops only the optional WiFi context provider.
// Signing, entitlements and notarized packaging belong to R01.

import SwiftUI

@main
struct SetupApp: App {
    @StateObject private var model = SetupModel()

    var body: some Scene {
        WindowGroup {
            ContentView(model: model)
                .onAppear { model.start() }
        }
        .windowResizability(.contentSize)
    }
}
