// SPDX-License-Identifier: GPL-3.0-only

import Foundation
import SetupCore
import SwiftUI

@MainActor
final class SetupModel: ObservableObject {
    nonisolated static let serviceLabel = "com.cydsystemmonitor.hostd"

    @Published var daemon: Result<DaemonStatus, ControlError>?
    @Published var service: ServiceState?
    @Published var permission: PermissionStatus = .notRequested
    @Published var providerConnection: ProviderConnection = .stopped
    @Published var disclosureShown = false
    @Published var message: String?
    @Published var confirmUnpair = false

    private let sensor = SystemWifiSensor()
    private lazy var runner = ProviderRunner(sensor: sensor)
    private var timer: Timer?
    private var ticks = 0

    var canRequestPermission: Bool {
        canRequestLocationPermission(disclosureShown: disclosureShown, status: permission)
    }

    /// Starts polling and the provider connection. Requests no permission.
    func start() {
        guard timer == nil else { return }
        sensor.onChange = { [weak self] in self?.permission = self?.sensor.authorization() ?? .error }
        permission = sensor.authorization()
        runner.start()
        poll()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    private func poll() {
        ticks += 1
        providerConnection = runner.connection
        Task.detached {
            let result: Result<DaemonStatus, ControlError>
            do {
                result = .success(try ControlClient().status())
            } catch let error as ControlError {
                result = .failure(error)
            } catch {
                result = .failure(.badReply)
            }
            await MainActor.run { self.daemon = result }
        }
        if ticks % 5 == 1 {
            Task.detached {
                let state = Self.readServiceState()
                await MainActor.run { self.service = state }
            }
        }
    }

    nonisolated private static func readServiceState() -> ServiceState {
        let plist = NSHomeDirectory() + "/Library/LaunchAgents/\(serviceLabel).plist"
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = ["print", "gui/\(getuid())/\(serviceLabel)"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()
        do {
            try process.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return ServiceState.parse(
                printOutput: String(decoding: data, as: UTF8.self),
                exitCode: process.terminationStatus,
                plistExists: FileManager.default.fileExists(atPath: plist))
        } catch {
            return ServiceState(
                installed: FileManager.default.fileExists(atPath: plist), loaded: false,
                running: false, pid: nil)
        }
    }

    /// The disclosure was put on screen; the request button may now work.
    func disclosureAppeared() { disclosureShown = true }

    /// Runs only from the button under the disclosure text.
    func requestLocationPermission() {
        guard canRequestPermission else { return }
        sensor.requestPermission()
    }

    func send(_ command: String) {
        Task.detached {
            let text: String
            do {
                try ControlClient().command(command)
                text = ""
            } catch ControlError.refused(_, let message) {
                text = message
            } catch {
                text = "The daemon did not accept the request."
            }
            await MainActor.run { self.message = text.isEmpty ? nil : text }
        }
    }
}
