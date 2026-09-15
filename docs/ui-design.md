<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# MVP interface model

The active E32R28T uses a calibrated 320×240 landscape touch interface with a dark, accessible theme. Color is always paired with text, icon, or shape; Alert severity colors are reserved for Alerts, while complementary accents distinguish navigation and data.

## Persistent status bar

Every screen keeps:

- the clock, or an honest `Time unavailable` state before the first post-boot time source;
- the highest-priority unacknowledged Alert and remaining Alert count;
- compact host/USB state; and
- the current WiFi or BLE Observation Window.

Tapping a status item opens the relevant detail. Alert priority sorts by Severity, then Confidence, then recency.

## Overview

Overview is the primary screen and contains six large expandable cards in a 2×3 grid:

- System;
- Storage;
- Network;
- Agents;
- WiFi; and
- BLE.

Cards use dials, animated state icons, and at most two or three concise numerical values such as network RX/TX. The MVP Overview has no graphs, sparklines, automatic rotation, or scrolling. Tapping a card opens its expanded screen; a clear back control returns to Overview. Alerts open from the persistent status bar, and Settings opens from a dedicated gear control.

## Stale host state

After three seconds without a one-second Host Metric snapshot, Host cards dim and show data age. Live gauges and animations decay toward their neutral minimum without claiming that a zero value was measured. The last numeric sample remains frozen and labeled `Last`, with Host Last Seen on Overview and expanded Host screens. After 15 seconds, the persistent bar shows the host disconnected while standalone radio monitoring continues if power remains available.

Expanded historical graphs remain a V1 feature. When they exist, a disconnect will freeze and dim their last dataset rather than erase it.

## Expanded screens

Expanded System, Storage, Network, and Agents screens show current detailed values, source age, Metric Availability reason, and relevant status lists; their historical graphs remain V1. Expanded Network and WiFi screens show Host WiFi Coverage as observable, outside supported coverage, or unknown; observable never implies continuous or current capture. The WiFi screen identifies its 2.4 GHz scope. Expanded WiFi and BLE screens follow the evidence and coverage views defined in [`radio-model.md`](radio-model.md).

## Alerts

A new Alert produces a persistent screen banner and RGB Severity signal. A short speaker sound is optional and disabled by default. Opening a Radio Alert shows its evidence and Confidence; opening an Operational Alert shows its concrete cause and affected subsystem with no Confidence field. Distinct type icons prevent operational faults from appearing to be wireless events. A separate acknowledge action removes banner and sound emphasis without ending an active episode or deleting it from history. During Coverage Gaps an active episode is labeled as currently unassessable. Recovery requires sufficient observed quiet time; reboot marks previously active episodes interrupted, preserves their last evidence, and starts fresh episodes only on new qualifying evidence. Interrupted must not be presented as recovered.

Operational Alerts cover protocol incompatibility/negotiation failure, radio initialization failure, persistent-state corruption or recovery loss, and repeated firmware crash/reset. Ordinary host disconnect, denied Location permission, and Operator-disabled radios remain visible status rather than Alerts. Operational Severity is Low for degradation of a nonessential capability, Medium for one monitoring subsystem unavailable, and High when host monitoring cannot negotiate or retained evidence is damaged. MVP uses only Low, Medium, and High. Extreme is deferred until the relevant detections and sufficient validation data support it; no version is assigned.

In V2, an idle missing SD card is also status. Failure to start an explicitly requested Survey Session, SD removal, insufficient space, corruption, or a write failure during recording creates an Operational Alert and stops the session without stopping ordinary monitoring.

## Setup and settings

First start verifies the hardware profile, runs five-point touch calibration and validation, sets basic display and sound preferences, shows USB host state, and then permits entry to Overview even without the daemon. First start does not block on a WiFi Channel Plan choice; the device starts observing on a world-safe default range. Settings provides normal recalibration and separate confirmed actions for erasing Radio History and performing a factory reset. A USB recovery command can clear unusable touch calibration.

Settings also provides independent persisted controls for enabling or disabling WiFi observation and BLE observation, plus a WiFi Channel Plan preference (a regional preset or the world-safe default) that the Operator may change at any time; it affects only which WiFi channels the scheduler dwells on and has no effect on BLE observation or on any radio's enabled state. A disabled radio must be labeled `Operator disabled`; it must never appear as measured zero activity or a Coverage Gap. Disabling a radio ends its active radio episodes with reason `configuration changed` but does not create an Alert or erase history. The remaining enabled radio receives the full scheduler budget; with both radios disabled, the device acts as a host-only monitor. Settings explains that the WiFi Channel Plan selects listening channels and does not establish legal compliance. Product documentation retains the general reminder that Operators are responsible for complying with their local law.

## About

The About screen supports interoperability troubleshooting. It displays at least:

- firmware version and build identity;
- hardware profile and active variant;
- firmware-supported protocol range;
- negotiated protocol version;
- protocol version advertised by the connected Host;
- Host daemon version when available;
- rule, configuration, persistence-schema, and calibration-schema versions;
- device uptime and last reset reason; and
- setup-app version/status when the Host provides it; V2 adds privileged-helper version/status.

If Host and firmware cannot negotiate a protocol, About and firmware diagnostics remain usable. The screen shows the firmware-supported range and raw incoming Host version, rejects incompatible Host data and configuration, and directs the Operator to update guidance.

## Pairing and time

The first valid USB handshake shows the Host label and a short matching code on both sides. Touch confirmation binds random Pairing Identities; unpairing stops Host exchange and returns immediately to standalone monitoring without deleting history. MVP supports one Host per Monitor and one Monitor per Host.

The Host supplies UTC, timezone, and uncertainty during handshake and periodically afterward. Alert ordering uses monotonic device time within a boot and durable ordering across boot identities. Significant corrections create a Time Discontinuity and never rewrite retained timestamps. Retained Host-impact evidence displays timing uncertainty; MVP has no Extreme escalation. Future Extreme timing limits require validation.
