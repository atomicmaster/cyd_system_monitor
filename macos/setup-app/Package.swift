// swift-tools-version:5.10
// SPDX-License-Identifier: GPL-3.0-only
import Foundation
import PackageDescription

// With only the Command Line Tools (no Xcode), the default build system does not
// pass the Swift Testing macro plugin to the compiler, so every other test build
// fails with "plugin for module 'TestingMacros' not found". Point the compiler at
// the plugin directory of whichever toolchain has one.
let testingPluginDirectories = [
    "/Library/Developer/CommandLineTools/usr/lib/swift/host/plugins/testing",
    "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/host/plugins/testing",
].filter { FileManager.default.fileExists(atPath: $0) }
let testingFlags = testingPluginDirectories.flatMap { ["-plugin-path", $0] }

let package = Package(
    name: "SetupApp",
    platforms: [.macOS(.v14)],
    targets: [
        // Pure, platform-neutral logic: wire models, permission mapping, the
        // provider protocol, bounded socket clients, status text.
        .target(name: "SetupCore", path: "Sources/SetupCore"),
        // The SwiftUI shell and the CoreLocation/CoreWLAN sensor.
        .executableTarget(
            name: "SetupApp",
            dependencies: ["SetupCore"],
            path: "Sources/SetupApp"
        ),
        // Swift Testing (XCTest is not available without Xcode).
        .testTarget(
            name: "SetupCoreTests",
            dependencies: ["SetupCore"],
            path: "Tests/SetupCoreTests",
            exclude: ["Fixtures"],
            swiftSettings: [.unsafeFlags(testingFlags)]
        ),
    ]
)
