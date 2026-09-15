<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# MVP radio model

The Monitor Device passively receives 2.4 GHz WiFi frames and BLE advertisements in explicit Observation Windows. It does not transmit probe or scan requests, perform Classic Bluetooth inquiry, retain raw payloads, or claim complete continuous coverage.

WiFi and BLE observation can be disabled independently. `Operator disabled` is an explicit state rather than zero activity or a Coverage Gap. Disabling a radio ends affected active Alerts with reason `configuration changed` without creating a new Alert, preserves Radio History, and reallocates the scheduler to any radio left enabled.

## Host coverage boundary

The E32R28T cannot observe 5/6 GHz WiFi. Host WiFi Coverage is observable, outside supported coverage, or unknown based on fresh band/channel context. Observable means the hardware supports that channel, not continuous or current capture; channel-plan exclusions and scheduled gaps remain visible. A matching SSID on a 2.4 GHz BSS does not establish evidence about a Host connection on another band. Host-targeted correlation requires relevant fresh captured evidence, while USB Host Metrics continue regardless of WiFi band.

Standalone observation requires power. Loss of Host software or the data connection does not stop it while powered; removing the sole USB power cable stops reception and triggers interruption recovery on the next boot.

Requested Observation Windows do not by themselves establish actual receiver availability. The M1a/M5 measurements must document switching overhead, loss, and coverage uncertainty; unavailable intervals cannot be counted as observed quiet time.

## Identity and labels

- WiFi density counts Observed BSSs by BSSID. Matching SSIDs may be grouped visually but do not imply one physical access point.
- The daemon always supplies the current Host Station Identity without macOS Location Services. With optional Location permission, it may also supply the current SSID/BSSID. The Monitor Device may infer a Host-associated BSS from fresh frames involving the station identity; permission, evidence, and coverage gaps remain explicit unknown states.
- BLE and other frame-source lists count Observed Transmitters by raw over-the-air address. An address is not assumed to be one physical object.
- SSIDs, advertised names, and decoded vendor/type attributes are untrusted Radio Labels. Rendering must escape them, and they never establish a physical owner.
- BLE lists order current and smoothed Signal Strength. They do not estimate distance or identify the nearest object.
- Documented iBeacon and Eddystone formats may use those names. Matching Apple advertisements are labeled `Find My-pattern advertisement`; they are not identified as AirTags, clones, or physical trackers.

## MVP views

The WiFi screen shows sampled-channel Observed WiFi Activity, Observed BSS count and list, management/control/data frame rates, top Observed Transmitters, and current coverage. The BLE screen shows advertisement density, Signal Strength ordering, decoded types, top Observed Transmitters, and current coverage. Tapping an item reveals retained attributes and related Alerts, never raw payload hex.

## MVP Alerts

The initial rule set contains:

- unusual deauthentication/disassociation rate;
- BLE advertisement flooding; and
- BLE identifier churn.

Ordinary discovery of a beacon type is a Radio Observation, not an Alert. Each rule creates bounded Alert episodes. Confidence is `low`, `medium`, or `high` and describes how strongly evidence supports the interpretation. MVP Severity is `low`, `medium`, or `high` and describes impact with known host/operator context.

An unusual deauthentication/disassociation rate starts at Medium and becomes High when fresh relevant evidence targets the Host Station Identity or Host-associated BSS. Direct station targeting may qualify without SSID/BSSID permission when the traffic is observable. BLE flood and churn Alerts are Low or Medium. Host link-loss or reassociation evidence may be retained with its age and time uncertainty, but MVP never emits Extreme.

Extreme is deferred to the release where all detections relevant to its claim are implemented and sufficient validation data supports it; no version is assigned. That release must establish Confidence, corroboration, timing/uncertainty limits, benign-event controls, and a false-Extreme acceptance gate.

Rules use versioned conservative thresholds normalized by Observed Time. Before fixing each rule's scenario expectations, document its evidence eligibility, minimum Observed Time and evidence counts, Confidence criteria, and observed-quiet recovery condition. Tracer bullets introduce these contracts as needed; complete rule/domain coverage remains part of M2 acceptance. Numeric thresholds are tuned and validated against M5/M6 measurements. Safe numeric thresholds are configurable, while rule logic changes with firmware.

## Episode lifecycle

- An active episode ends through recovery only after sufficient relevant observed quiet time. Coverage Gaps pause evidence and recovery evaluation, do not end the episode, and make its current condition unassessable.
- Operator configuration changes may end affected episodes with reason `configuration changed`; this does not claim observed recovery.
- Acknowledgment marks an episode seen without changing its lifecycle or evidence.
- Reboot recovery preserves committed evidence and marks previously active episodes `interrupted`. It does not infer quiet time, a recovery time, or the exact power-loss time. Fresh qualifying evidence starts a new episode.
- Ended and interrupted episodes preserve peak Severity, first/last evidence, and any recorded impact. Recovery time exists only when recovery was observed.

## History and export

Initial internal-flash targets are 32 detailed active Alerts, 128 ended/interrupted Alerts, and 512 least-recently-used Transmitter Baselines. These are provisional until M1a establishes safe capacities; M6 validates them with maximum record sizes, RAM use, partition space, journal reclamation, checkpoint/write rates, and endurance assumptions. Record the resulting enforced limits before release. Reserve space for Operational and aggregate Capacity-pressure Alerts so detailed matches cannot crowd out failure reporting. Excess active matches are coalesced by rule into Capacity-pressure Alerts. Radio History may be exported only by an explicit USB CLI action with on-screen disclosure; it is never mirrored automatically.

## Release evidence

Controlled replay must trigger every rule at its documented boundary and exercise observed-quiet recovery, Coverage Gaps, acknowledgment, and reboot interruption. Benign controls cover normal roaming, intentional WiFi disconnects, Host sleep/wake, and ordinary BLE address changes; these controls must not be interpreted as hostile Host impact. A 72-hour ordinary desk soak must produce zero known-false medium/high Alerts and at most three known-false low Alerts per rule. End-to-end validation also includes host-native rule/UI-model tests, shared USB protocol golden vectors, power-loss journal recovery, daemon fixtures, hardware smoke tests, USB reconnects, and daemon restarts. Privileged-helper validation belongs to V2; MVP scenarios must confirm that Extreme is never emitted.
