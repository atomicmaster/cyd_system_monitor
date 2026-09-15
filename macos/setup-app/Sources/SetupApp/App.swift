// SPDX-License-Identifier: GPL-3.0-only
//
// Development-build SwiftUI status/setup app. Pairing status, permission
// workflow, and the Location/Association Context flow are TB05/TB08 work.
// Notarized release packaging belongs to R01.

import SwiftUI

@main
struct SetupApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("cyd_system_monitor setup")
                .font(.headline)
            Text("Development build. No LaunchAgent installed, no Location permission requested.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(24)
        .frame(width: 380, height: 140, alignment: .topLeading)
    }
}
