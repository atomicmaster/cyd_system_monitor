<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# MVP interface model

The active E32R28T uses a calibrated 320×240 landscape touch interface with a dark, accessible theme. Color is always paired with text, icon, or shape; Alert severity colors are reserved for Alerts, while complementary accents distinguish navigation and data.

The palette ([`theme.hpp`](../firmware/ui/include/firmware/ui/theme.hpp)) is deep navy surfaces with rounded, thinly bordered panels, muted captions, and cool neon data accents: cyan, violet, blue, teal, pink and magenta. Red, orange, amber and yellow never appear as data accents, so no gauge or value can read as an Alert. The one deliberate exception is the Claude agent mascot, drawn in Claude Code's own terracotta and always with its crab shape; it is an identity, not a value.

Paired rates share two colours everywhere: pink for the first (download, storage read) and cyan for the second (upload, storage write).

The chrome follows a quiet style: no filled buttons (actions are coloured text, cyan for the expected one), hairlines instead of boxes where a box adds nothing, and overlays as sheets rising from the bottom edge with a grab handle. Repeated styles are shared LVGL styles, so each costs pool memory once.

## Persistent status bar

Every screen keeps:

- the clock, or an honest `Time unavailable` state before the first post-boot time source;
- the highest-priority unacknowledged Alert and remaining Alert count;
- compact host/USB state; and
- the current WiFi or BLE Observation Window.

Live Host data shows as a cyan USB glyph rather than the word; every other connection state is spelled out. A hairline separates the bar from the cards. The bar is one 12 pt line, and every combination of its longest texts must fit its 312 px ("Disconnected", "WiFi gap BLE scan", "Time unavailable"; checked by the simulator's `system-screen` scenario). That is why its words are short ("Waiting", "Incompatible", `err` for a faulted radio) and the clock has no zone suffix. When the zone is unknown the clock still says `UTC`, so it can never pass as local time.

Tapping the connection text opens the Host link sheet (versions, pairing, and any link fault, as label/value rows built only while it is open); tapping the rest of the bar opens the clock settings sheet. Tapping a status item opens the relevant detail. Alert priority sorts by Severity, then Confidence, then recency.

## Overview

Overview is the primary screen. Under the status bar it shows the Host Metric cards, and along the bottom the Agents bar:

- **dials**, three to a row: CPU, RAM (Physical RAM Occupied) and Storage. Each is a 270° gauge with its share in the centre and one short line under it: used / total for RAM, and for Storage (the startup volume's used share) the read and write totals over every physical disk in two fixed slots (a pink `R` and a cyan `W`, each followed by its value in its own box), so the line does not shift as the numbers change;
- **panels**, two to a row, each led by a glyph instead of a title:
  - **Battery:** a drawn cell filled to the charge, with a bolt while on power; the charge and the time left (`5h12m left`, `1h15m to full`) or the state beside it; and a health line underneath (`Health`, `Service` or `Replace`, a bar and the maximum capacity). A retained charge greys the fill rather than emptying it, since an empty cell would read as a flat battery; a retained health is captioned `Last`;
  - **Network:** the link line on top: one icon per Active Physical Link (the WiFi glyph, a drawn Ethernet port, or two joined nodes for another link; at most three, then `+N`), followed by `VPN` when the primary route is a tunnel and the Host WiFi coverage (`ch6 2.4G`, `5/6G unseen`; shortened to `ch6`, `5/6G` beside other icons) when the Host knows it, or `Offline` when there is no route. Then a pink download row and a cyan upload row, the totals over those links, each a value and a log-scale meter (100 B/s to 1 GB/s) so a few KB/s and tens of MB/s both register. Retained rates keep their values, drop their meters, and the link line leads with `Last` and gives up the glyph unless it would be left with no words; and
- **the Agents bar**: each agent's mascot in a fixed third of the bar, with no words in the normal states. Not running is a dim silhouette; running or idle is the mascot in its own colour, still; busy animates it (Claude's crab dances, Codex's `>_` cursor blinks, the Grok mark rocks). Busy comes from the Host's activity sources ([agent-integrations.md](agent-integrations.md)); a running agent with no such source is simply still. When processes cannot be listed the bar shows `--` and the cause and no mascot, so a failure never reads as nothing running. A retained reading is dimmed, labelled `Last`, and every mascot stops. A link fault takes the bar's place, so its cause stays visible.

Cards come from the card registry in order, and a card's view chooses dial or panel, so adding a metric needs no layout change while the grid has room. WiFi and BLE have no Overview view yet; their place is open until the radio slices (M5) define their summaries. The whole Overview uses about 43 KB of the board's 57 KB LVGL pool steadily and about 48 KB at peak (about 50 KB with the Host link sheet open), measured by `./dev run simulator -- preview`. Cards use dials, animated state icons, and at most two or three concise numerical values such as network RX/TX. The MVP Overview has no graphs, sparklines, automatic rotation, or scrolling. Tapping a card opens its expanded screen; a clear back control returns to Overview. Alerts open from the persistent status bar, and Settings opens from a dedicated gear control.

## Stale host state

After three seconds without a one-second Host Metric snapshot, Host cards dim and show data age. Live gauges and animations decay toward their neutral minimum without claiming that a zero value was measured. The last numeric sample remains frozen and labeled `Last`, with Host Last Seen on Overview and expanded Host screens. After 15 seconds, the persistent bar shows the host disconnected while standalone radio monitoring continues if power remains available.

Expanded historical graphs remain a V1 feature. When they exist, a disconnect will freeze and dim their last dataset rather than erase it.

## Expanded screens

Expanded System, Storage, Network, and Agents screens show current detailed values, source age, Metric Availability reason, and relevant status lists; their historical graphs remain V1. Expanded Network and WiFi screens show Host WiFi Coverage as observable, outside supported coverage, or unknown; observable never implies continuous or current capture. The WiFi screen identifies its 2.4 GHz scope. Expanded WiFi and BLE screens follow the evidence and coverage views defined in [`radio-model.md`](radio-model.md).

## Alerts

A new Alert produces a persistent screen banner and RGB Severity signal. Opening a Radio Alert shows its evidence and Confidence; opening an Operational Alert shows its concrete cause and affected subsystem with no Confidence field. Distinct type icons prevent operational faults from appearing to be wireless events. A separate acknowledge action removes banner emphasis without ending an active episode or deleting it from history. During Coverage Gaps an active episode is labeled as currently unassessable. Recovery requires sufficient observed quiet time; reboot marks previously active episodes interrupted, preserves their last evidence, and starts fresh episodes only on new qualifying evidence. Interrupted must not be presented as recovered. Optional speaker alerts are deferred to [FR01](tickets/FR01.md).

Operational Alerts cover protocol incompatibility/negotiation failure, radio initialization failure, persistent-state corruption or recovery loss, and repeated firmware crash/reset. Ordinary host disconnect, denied Location permission, and Operator-disabled radios remain visible status rather than Alerts. Operational Severity is Low for degradation of a nonessential capability, Medium for one monitoring subsystem unavailable, and High when host monitoring cannot negotiate or retained evidence is damaged. MVP uses only Low, Medium, and High. Extreme is deferred until the relevant detections and sufficient validation data support it; no version is assigned.

In V2, an idle missing SD card is also status. Failure to start an explicitly requested Survey Session, SD removal, insufficient space, corruption, or a write failure during recording creates an Operational Alert and stops the session without stopping ordinary monitoring.

## Setup and settings

First start verifies the hardware profile, runs five-point touch calibration and validation, sets basic display preferences, shows USB host state, and then permits entry to Overview even without the daemon. First start does not block on a WiFi Channel Plan choice; the device starts observing on a world-safe default range. Settings provides normal recalibration and separate confirmed actions for erasing Radio History and performing a factory reset. A USB recovery command can clear unusable touch calibration.

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

The first valid USB handshake shows the Host label and a short matching code on both sides; the display shows the code large, in spaced cyan digits, with Confirm and Decline as text actions. Touch confirmation binds random Pairing Identities; unpairing stops Host exchange and returns immediately to standalone monitoring without deleting history. MVP supports one Host per Monitor and one Monitor per Host.

The Host supplies UTC, timezone, and uncertainty during handshake and periodically afterward. Alert ordering uses monotonic device time within a boot and durable ordering across boot identities. Significant corrections create a Time Discontinuity and never rewrite retained timestamps. Retained Host-impact evidence displays timing uncertainty; MVP has no Extreme escalation. Future Extreme timing limits require validation.
