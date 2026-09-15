<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Isolate V2 Apple activity diagnostics behind a privileged helper

The planning review defers GPU/ANE diagnostics and their privileged helper to V2, including installation/removal, status UI, packaging, and validation. They are not MVP or V1 requirements.

On supported Macs in V2, a separate Rust root helper will run only Apple's fixed `/usr/bin/powermetrics` path with the `gpu_power,ane_power` sampler allowlist, parse its five-second NUL-separated plist stream, and publish sanitized samples over restricted local IPC. It may share diagnostic types and plist fixtures with the normal Rust daemon but never a general command runner. The daemon remains a per-user LaunchAgent and continues when the helper is absent or fails. Release installation and removal require an explicit administrator-authorized, signed and notarized package; development uses documented `sudo` commands. V2 must validate sampler availability and parser fixtures on its supported macOS versions before treating the proposed fields and cadence as supported. This accepts installation and maintenance cost to expose explicitly labeled Apple Activity Diagnostics without granting the main daemon arbitrary root execution or claiming portable GPU/NPU utilization.
