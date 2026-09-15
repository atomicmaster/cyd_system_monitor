// swift-tools-version:5.10
// SPDX-License-Identifier: GPL-3.0-only
import PackageDescription

let package = Package(
    name: "SetupApp",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "SetupApp", path: "Sources/SetupApp")
    ]
)
