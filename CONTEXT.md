<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# CYD System Monitor

This context describes a desk display that presents the state of one paired computer and ambient observations of the nearby WiFi and Bluetooth environment to a technical operator.

## People and Equipment

**Operator**:
The technical user who pairs the monitor with a host and interprets its metrics and alerts.
_Avoid_: User

**Monitor Device**:
The physical Cheap Yellow Display that presents host metrics and observes the nearby radio environment.
_Avoid_: Device, CYD device

**Monitored Host**:
The single computer explicitly paired with the Monitor Device whose operating state is displayed.
_Avoid_: Local system, System, Machine

**Pairing Identity**:
A random persistent identifier assigned by a Monitor Device or Monitored Host and confirmed over physical USB; it identifies the intended peer but is not a cryptographic secret.
_Avoid_: Hostname, USB path, Pairing key

**Radio Environment**:
The WiFi and Bluetooth transmissions observable near the Monitor Device.
_Avoid_: Local airwaves, Network

## Monitoring

**Host Metric**:
A measured aspect of the Monitored Host's current or historical operating state.
_Avoid_: System metric, Telemetry

**Physical RAM Occupied**:
Total physical memory minus free pages, including reclaimable caches; it is a host-local headline rather than a cross-platform pressure comparison.
_Avoid_: App memory, Memory pressure

**Apple Activity Diagnostic**:
A V2 source-specific estimate of GPU or Apple Neural Engine power, frequency, and duty or residency data reported by macOS; it is not a portable utilization percentage.
_Avoid_: GPU utilization, NPU utilization

**Primary Network Interface**:
The active non-loopback interface selected by the Monitored Host's primary route. Tunnel and underlying-interface rates are shown separately and are never summed.
_Avoid_: Total network interface

**Host Station Identity**:
The current over-the-air link-layer address of the Monitored Host's active WiFi interface, which may be a private randomized address rather than its factory hardware address.
_Avoid_: Hardware MAC, Permanent MAC

**Host WiFi Coverage**:
The relationship between the Monitored Host's current WiFi band/channel and the Monitor Device's observation capability: observable, outside supported coverage, or unknown. Observable does not imply that the channel is currently sampled or continuously covered.
_Avoid_: Protected connection, Safe network

**Host-associated BSS**:
An Observed BSS associated with the Monitored Host through fresh OS-reported BSSID context or fresh WiFi frames involving the Host Station Identity; the relationship is unknown when permission, evidence, or coverage is insufficient.
_Avoid_: Current network, Location

**Association Context Availability**:
The reason current SSID/BSSID context is or is not usable: available, permission not requested, denied, restricted, authorized but unavailable, unsupported, or error.
_Avoid_: Location status, Connected network missing

**Metric Availability**:
The state explaining whether a Host Metric has a value: available, unsupported by the platform, absent from the host, temporarily unavailable, or failed with an error.
_Avoid_: Missing value, N/A

**Metric Freshness**:
Whether a retained Host Metric sample is fresh or stale for its expected refresh cadence, independently of Metric Availability or whether its adapter is enabled.
_Avoid_: Availability, Measured zero

**Host Last Seen**:
The wall-clock date and time of the most recent valid Host Metric snapshot received from the Monitored Host.
_Avoid_: Last metric time, Disconnect time

**Time Discontinuity**:
A recorded correction in the relationship between monotonic device time and UTC. Existing evidence keeps its original resolved timestamp and is never reordered or rewritten after a correction.
_Avoid_: Missing sample, Clock reset

**Host History**:
The bounded, 30-day multiresolution series of Host Metrics owned by the Host daemon, with explicit stale and unavailable durations preserved during compaction.
_Avoid_: Radio History, Raw telemetry archive

**Live Indicator**:
A dial, animated icon, or concise numeric value representing current state. When its source becomes stale, its motion or gauge decays to neutral while the final measured value remains labeled as historical evidence.
_Avoid_: History graph

**Radio Observation**:
Evidence derived from passively received WiFi or Bluetooth transmissions.
_Avoid_: Detection, Attack

**Radio Label**:
Untrusted human-readable text or a decoded vendor/type attribute advertised by an Observed Transmitter; it identifies a transmission, not a physical owner.
_Avoid_: Device owner, Verified name

**Signal Strength**:
Current or smoothed received signal strength used to order Observed Transmitters; it does not establish distance or physical proximity.
_Avoid_: Distance, Nearest device, Proximity

**Observed WiFi Activity**:
The frame rate and estimated airtime represented by WiFi frames received during known Observation Windows; it does not claim total RF occupancy or congestion.
_Avoid_: Channel occupancy, Congestion

**Observed BSS**:
A WiFi basic service set identified by a received BSSID. It does not necessarily correspond one-to-one with a physical access point, and a shared SSID does not make several BSSIDs one identity.
_Avoid_: Access point, WiFi device

**Observed Transmitter**:
A radio identifier observed in the Radio Environment, which may or may not correspond uniquely to one physical object.
_Avoid_: Hardware device, Nearby device

**Transmitter Baseline**:
The remembered attributes and observation summary for an Observed Transmitter against which later Radio Observations are compared.
_Avoid_: Device profile, Known device

**Observation Window**:
A scheduled interval in which the Monitor Device observes one radio mode or channel.
_Avoid_: Continuous scan

**Coverage Gap**:
An interval or radio channel the Monitor Device did not observe and about which it can make no claim.
_Avoid_: Clear, Safe

**Observed Time**:
The accumulated duration of relevant Observation Windows used to evaluate a radio rule; Coverage Gaps do not advance it.
_Avoid_: Wall-clock window

**Alert**:
A bounded episode requiring Operator attention, with first-seen and last-seen times, Severity, evidence or concrete cause, and active, ended, or interrupted state. An interruption preserves evidence without claiming observed recovery; acknowledgment marks the episode seen without altering its evidence or lifecycle.
_Avoid_: Notification, Status

**Radio Alert**:
An Alert interpreting a suspicious pattern in Radio Observations, with independent Severity and Confidence.
_Avoid_: Detection, Confirmed attack

**Operational Alert**:
An Alert reporting a concrete monitoring or compatibility fault with Severity and cause; Confidence does not apply.
_Avoid_: Radio Alert, Unavailable status

**Capacity-pressure Alert**:
An aggregate Radio Alert indicating that detailed active-Alert capacity was exhausted, with counts and peak evidence grouped by rule.
_Avoid_: Dropped Alert

**Severity**:
The potential impact of an Alert if its interpretation is correct, expressed as low, medium, or high in the MVP. Extreme is a deferred category requiring relevant detections and validated corroboration of impact on the Monitored Host or Operator before release.
_Avoid_: Confidence, Frame count

**Confidence**:
The strength with which the available Radio Observations support an Alert's interpretation.
_Avoid_: Severity, Signal strength

**Acknowledgment**:
The Operator's indication that an Alert has been seen; it does not end the Alert or alter its evidence.
_Avoid_: Dismissal, Resolution

**Agent Activity**:
The observed execution state of a supported local agent: not running, running with activity unknown, busy, or idle. Busy and idle require evidence supplied by an agent-specific adapter.
_Avoid_: Agent task, Agent usage, Process activity

**Agent Adapter**:
An explicitly enabled Host component that derives bounded Agent Activity or usage fields from a documented local API, structured log, or agent-provided hook and declares exactly what it reads and retains.
_Avoid_: Conversation scraper, Process heuristic

**Radio History**:
The retained sequence of derived Radio Observations and Alerts owned by the Monitor Device.
_Avoid_: Packet capture, Forensic capture

**WiFi Channel Plan**:
The Operator-configurable range of WiFi channels the scheduler dwells on during Observation Windows, such as a regional preset or a world-safe default. It is a scheduling preference rather than a legal permission the Monitor Device enforces, and it has no effect on BLE observation.
_Avoid_: Regulatory Region, Locale, Time zone

**Position Fix**:
A timestamped latitude, longitude, altitude, and accuracy estimate supplied by an optional GPS receiver attached to the Monitored Host. It is eligible for survey export only while fresh and within the configured accuracy limit.
_Avoid_: Location permission, Current location

**Survey Session**:
An explicitly started and stopped V2 recording interval that combines eligible Position Fixes with selected WiFi and/or BLE Radio Observations and produces one or more immutable WiGLE-compatible artifacts on the Monitor Device's SD card.
_Avoid_: Radio History, Automatic wardrive

**Survey Artifact**:
An immutable, checksum-addressed WiGLE Wireless CSV gzip file produced by a Survey Session, with separate local index metadata for discovery, resumable transfer, and deletion.
_Avoid_: Database, Upload, Export copy
