<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Store GPS surveys as Device-owned WiGLE artifacts

V2 GPS-assisted wardriving uses explicitly started Survey Sessions owned by the Monitor Device. An optional USB GPS receiver attaches to the Monitored Host, whose Rust NMEA 0183 adapter supplies timestamped Position Fixes; this accommodates the E32R28T's USB-device-only connector while leaving radio observation and session persistence under firmware control. The Operator selects WiFi, BLE, or both, and the persistent status area shows recording and GPS-fix state.

The Monitor Device writes append-only UTF-8 WiGLE Wireless CSV 1.6 work files to SD card, omitting survey rows whose Position Fix is invalid, older than five seconds, or less accurate than 50 meters by default. On stop, size rotation, or power-loss recovery it validates complete rows, creates and verifies an immutable `.csv.gz` Survey Artifact, and indexes its time range, bounds, row count, byte size, SHA-256, and transfer state. Ordinary radio monitoring continues when survey rows are omitted.

The Host retrieves original artifact bytes with resumable, checksum-verified transfer and may open WiGLE's manual upload workflow. It does not automatically upload, infer that a download was uploaded, or store WiGLE credentials. First recording and first download disclose that the artifacts pair precise coordinates and times with SSIDs, MAC addresses, BLE addresses, and decoded labels. Copies on the Monitor Device and Monitored Host are deleted independently and explicitly.
