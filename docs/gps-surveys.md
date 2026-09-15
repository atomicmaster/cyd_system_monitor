<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# GPS-assisted survey design

This V2 feature creates WiGLE-compatible wardrive files without making coordinates part of normal Radio History. The compatibility facts and release fixtures are documented in [research/wigle-format.md](research/wigle-format.md).

## Data path

1. An optional USB GPS receiver connects to the Monitored Host.
2. A Rust serial adapter parses NMEA 0183 and sends timestamped Position Fixes over the existing USB protocol.
3. The Operator explicitly starts a Survey Session on the Monitor Device or through an equivalent Host command, selecting WiFi, BLE, or both.
4. Firmware pairs each selected Radio Observation with the closest valid Position Fix in monotonic time. The initial eligibility gate requires a fix no older than five seconds and an accuracy estimate no worse than 50 meters. Ineligible observations remain available to ordinary monitoring but do not become survey rows.
5. For each Observed Transmitter, firmware retains the first eligible observation and another when at least five seconds have elapsed, position has changed by at least 10 meters, or Signal Strength has changed by at least 8 dB. It also retains the final eligible observation. These are field-tunable volume controls rather than identity deduplication.
6. Firmware writes an append-only UTF-8 WiGLE Wireless CSV 1.6 `.partial` file on SD card and periodically synchronizes it. Stop, rotation, or recovery validates complete rows, compresses to a new gzip file, verifies it, and marks it immutable.

The persistent display area shows whether recording is active and whether GPS is valid, stale, inaccurate, or absent. Session start and stop are deliberate configuration actions and therefore do not create Alerts.

## Artifacts and transfer

One session produces one or more immutable `WigleWifi_*.csv.gz` Survey Artifacts. Firmware starts another numbered part at 128 MiB compressed, after 24 hours, or when the UTC date changes, whichever occurs first. A conservative work-file threshold and final verification ensure no completed part exceeds the compressed limit. Every part carries the same stable session identity and its own part identity and checksum. A sidecar index records start and end UTC, geographic bounds, row count, byte size, SHA-256, and transfer state without changing the WiGLE payload.

The Host application can list artifacts and resume download of their original bytes. It verifies SHA-256 before presenting a completed local copy. Download is not upload state. The application offers a shortcut to WiGLE's manual upload page and keeps WiGLE accounts and credentials outside the product.

## Privacy and deletion

Before first recording and first download, the interface explains that a Survey Artifact combines exact coordinates and times with SSIDs, WiFi MAC addresses, BLE addresses, and decoded labels. Raw addresses remain necessary for WiGLE compatibility even though radio transmitters expose them over the air. The durable time-and-position combination is treated as sensitive data.

Deleting an SD-card artifact and deleting a downloaded Host copy are separate explicit actions with confirmation. Firmware refuses deletion while that artifact is being transferred. Neither action claims to remove a file already submitted to WiGLE or copied elsewhere.

## Storage failures

An absent or unusable SD card while no Survey Session is active is a visible status. Attempting to start without usable storage, removal during recording, a write failure, corruption, or insufficient space during recording creates an Operational Alert and safely stops the Survey Session. Host and radio monitoring continue.
