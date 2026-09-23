// SPDX-License-Identifier: GPL-3.0-only

import SetupCore
import SwiftUI

// `@State` is a macro in current SDKs and needs Xcode's SwiftUIMacros plugin,
// absent from the Command Line Tools; view state lives in the model instead.
struct ContentView: View {
    @ObservedObject var model: SetupModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("CYD System Monitor setup").font(.headline)
            Text(StatusText.service(model.service))
            Text(StatusText.daemon(model.daemon))
            if case .success(let status)? = model.daemon {
                pairingSection(status)
                let lines = StatusText.metrics(status.metrics)
                if !lines.isEmpty {
                    Text(lines.joined(separator: "   ")).font(.caption).foregroundStyle(.secondary)
                }
                if let drops = StatusText.drops(status.counters) {
                    Text(drops).font(.caption).foregroundStyle(.secondary)
                }
                Text(StatusText.permission(status.wifiContext)).font(.callout)
            }
            Divider()
            permissionSection
            if let message = model.message {
                Text(message).font(.caption).foregroundStyle(.red)
            }
        }
        .padding(24)
        .frame(width: 460, alignment: .topLeading)
    }

    @ViewBuilder
    private func pairingSection(_ status: DaemonStatus) -> some View {
        Text(StatusText.link(status.link))
        HStack {
            switch status.link.state {
            case .pairingRequired where status.link.pairingCode != nil:
                Button("Cancel pairing") { model.send("pair_cancel") }
            case .pairingRequired:
                Button("Start pairing") { model.send("pair_start") }
            case .pairedConnected:
                Button("Unpair…") { model.confirmUnpair = true }
            default:
                EmptyView()
            }
        }
        .confirmationDialog(
            "Unpair the display?", isPresented: $model.confirmUnpair, titleVisibility: .visible
        ) {
            Button("Unpair", role: .destructive) { model.send("unpair") }
        } message: {
            Text("The display forgets this Mac. Histories are kept.")
        }
    }

    private var permissionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Optional: WiFi network identity").font(.subheadline).bold()
            Text(Disclosure.text)
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
                .onAppear { model.disclosureAppeared() }
            HStack {
                Button("Request Location permission") { model.requestLocationPermission() }
                    .disabled(!model.canRequestPermission)
                Text("Skip: monitoring continues without it.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Text("This window's provider: \(providerText)")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var providerText: String {
        switch model.providerConnection {
        case .connected: return "sending WiFi context to the daemon"
        case .connecting: return "waiting for the daemon"
        case .stopped: return "stopped"
        }
    }
}
