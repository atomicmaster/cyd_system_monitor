<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# MVP Alert rule specifications

This document fixes, for each MVP Radio Alert rule, the evidence eligibility, Observed Time normalization, minimum Observed Time and evidence counts, Severity and Confidence criteria, observed-quiet recovery, and lifecycle edge cases. [radio-model.md](radio-model.md), [ADR 0015](adr/0015-use-deterministic-coverage-aware-rules.md) and M2 in [IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md) require this before any scenario expectation is fixed. Fixture expectations below are derived from it, not the reverse.

**Status of numbers.** Every numeric value marked *candidate* is a design placeholder chosen to under-alert. None is a measurement. The repo has no recorded packet-loss, dense-environment, or attack-traffic data: [feasibility.md](../hardware/profiles/lcdwiki-esp32-32e-2.8/development/feasibility.md) records only switch timing, one office's beacon counts, and BLE reports of about 36 per second in one office (3,896 - 436 reports over 96 s, continuous scan, before any scheduler). M5/M6 recordings confirm or replace each number listed under "Open measurements". Safe numbers are configurable; changing a threshold is a configuration change (see Lifecycle).

| Rule | Version string | Radio | Severity emitted |
| --- | --- | --- | --- |
| Deauthentication/disassociation rate (TB12) | `deauth-rate/1` | WiFi | Medium, High (never Low, per radio-model) |
| BLE advertisement flooding (TB14) | `ble-flood/1` | BLE | Low, Medium |
| BLE identifier churn (TB15) | `ble-churn/1` | BLE | Low, Medium |

No rule emits Extreme.

**Version convention.** `<rule>/<n>`. Increment `n` whenever eligibility, normalization, window structure, hysteresis logic, Confidence criteria or a default threshold changes. Editing a configured threshold does not change `n`. An episode stores its rule version and the thresholds in force when it opened; existing episodes are never re-evaluated under a newer version (after a firmware update the reboot rule applies).

## 1. Shared definitions

Terms follow [CONTEXT.md](../CONTEXT.md). The rule engine consumes only `WindowReport`s and observation events from the TB09/TB10/TB11 interface ([extension-interfaces.md](extension-interfaces.md), "Radio observation"); it reads no clock and invents no timestamp.

### 1.1 Eligible window

A window contributes Observed Time and evidence only if it is an *eligible window*:

- its `WindowReport.observed_ms` is at least half of `planned_ms`. Less is a Coverage Gap (TB09): it contributes zero Observed Time and every event attributed to it is ineligible;
- the radio state is observing (not `operator-disabled`, `fault`, or `no-data`); and
- its observed time comes from the driver report. Requested time is never used, and an event whose window has no report is ineligible ("unknown observed time").

Rules use per-window `observed_ms`, not `CoverageLedger::totals`, which include partial time from gap windows.

A window with `dropped_events > 0` is eligible and its events count, but its counts are a lower bound. It may help open an episode (this under-alerts) but it never advances a recovery clock: overload is not observed quiet. Drops are shown as context, never counted as evidence.

### 1.2 Observed Time by radio

- WiFi: Observed Time is kept **per channel**, the sum of `observed_ms` of that channel's eligible windows. With the default plan (channels 1-11, 205 ms dwell) a channel is dwelled 205 ms per 2.3 s revisit, about 8.9% of wall time, or per 4.5 s (about 4.6%) when BLE is enabled. Rates are never pooled across channels; a per-channel rate divides that channel's count by that channel's Observed Time only.
- BLE: Observed Time is the sum of `observed_ms` of eligible BLE windows. Enabled together with WiFi, BLE gets 205 ms per 410 ms (50% duty); alone it gets every window.

Consequence: an evaluation window measured in Observed Time spans much more wall time than its length (2.0 s of WiFi channel Observed Time is at least 10 dwells, about 23 s of wall time WiFi-only or 45 s with BLE). Short bursts between visits are missed. That is accepted; it under-alerts.

### 1.3 Evaluation cadence and staleness

Rules evaluate when an eligible window report arrives for their radio (and channel, for WiFi). Time does not advance an evaluation. If a rule's stream has no eligible window for more than `kStaleRevisitPeriods` (3) expected revisit periods (`coverage.hpp`; for BLE, the expected period between BLE windows, which is the window plus the interleaved WiFi dwell when both radios are enabled), its trailing evidence is cleared and an active episode is shown as currently unassessable ([ui-design.md](ui-design.md), Alerts). The episode is not ended and no quiet accrues. Unknown is never quiet.

### 1.4 Ordering and provenance

Evidence is ordered by `(boot_identity, sequence)`, where `sequence` is a per-boot monotonic counter assigned when an eligible event is accepted ([extension-interfaces.md](extension-interfaces.md), "Episode evidence"; persistence ordering is finalized in TB13). An episode records:

| Field | Meaning |
| --- | --- |
| `rule_version`, thresholds in force | provenance |
| `first_evidence` | key of the earliest eligible event in the evaluation that opened the episode |
| `last_evidence` | key of the latest eligible event counted toward the episode |
| `peak_evidence` | key and summary (count, Observed Time, rate or distinct count) of the evaluation with the highest rate or count; the earliest wins ties |
| peak Severity, current Confidence | see rule sections |
| state | Active, Ended (reason `observed recovery` or `configuration changed`), Interrupted |
| acknowledged flag | display only |

UTC, when available, is display context resolved at record time and is never an ordering key. Without resolved time the UI shows `Time unavailable`.

### 1.5 Lifecycle common to all rules

| Event | Behavior |
| --- | --- |
| Open | All minimums of the rule section met at one evaluation. Below any minimum: no episode, regardless of rate. |
| Escalate | Severity is raised when a higher tier's conditions are met; it never lowers within an episode, so current Severity equals peak Severity. Confidence is re-evaluated and may rise or fall while active (final and peak Confidence are both retained). |
| Hold band | Between the recovery (lower) threshold and the open threshold: the episode stays Active, its recovery clock is reset to zero, and no new Alert is created. |
| Observed-quiet recovery | The recovery clock accrues the `observed_ms` of each consecutive eligible, zero-drop window whose trailing evaluation is below the lower threshold. Reaching the required Observed Time ends the episode with reason `observed recovery`. Any evaluation at or above the lower threshold resets the clock to zero. Ineligible windows (Coverage Gaps, drops, no-data) neither accrue nor reset it. If the rule's stream has no eligible window for the recovery-clock maximum pause (candidate: 10 minutes of monotonic device time), accrued quiet is discarded (the gap may have hidden the anomaly), but the episode stays Active. After a stale clear (1.3) quiet accrues only once the trailing window has been refilled with eligible Observed Time. |
| Acknowledgment | Sets the acknowledged flag. Never ends, alters evidence, or shortens recovery. |
| Schedule or configuration change | Any change to the rule's radio configuration (enable, WiFi Channel Plan, dwell or window length) or to a rule threshold ends that radio's affected active episodes with reason `configuration changed` (deauth: episodes of channels affected; BLE: all BLE-rule episodes), resets its trailing evidence and recovery clocks, and emits no Alert. Continuing anomaly evidence may open a new episode only after the full minimums are met again. Toggling the *other* radio only reassigns budget: episodes and trailing evidence are kept. |
| Reboot | Trailing evidence, recovery clocks and identifier tables are RAM only and are lost. Committed active episodes return as `interrupted`, never as recovered, with no invented quiet or outage time. New qualifying evidence opens a **new** episode after the full minimums (and warm-up for `ble-churn/1`) are met again. |
| Time Discontinuity | No effect on thresholds, windows, recovery or ordering (all use Observed Time and `(boot, sequence)`). It is recorded as an annotation on active episodes; existing evidence keeps its resolved timestamp and is never reordered. |
| Capacity pressure | If the detailed active-Alert store refuses an opening, the rule updates one O(1) aggregate per rule (suppressed-opening count, peak Severity, peak count/rate, first/last evidence keys) that feeds the Capacity-pressure Alert. Rules never grow state per suppressed episode. |
| Sort key | Alerts sort by Severity, then Confidence, then recency ([ui-design.md](ui-design.md)). |

### 1.6 Confidence, common

Confidence (low, medium, high) is independent of Severity and describes how strongly evidence supports the rule's interpretation. Inputs: eligible evidence count, persistence across separate windows, observed share (`observed_ms` over `planned_ms` of the episode's windows, gap windows included in the denominator), and corroboration. Higher Confidence never raises Severity and Severity never raises Confidence.

## 2. `deauth-rate/1` (TB12)

### 2.1 Evidence eligibility

| Eligible | Ineligible |
| --- | --- |
| 802.11 management frames subtype 12 (deauthentication) and 10 (disassociation), FCS-valid, header complete, on a channel in an eligible window, retry bit clear. Directed (receiver is a unicast address) and broadcast/group-addressed frames both count. Protected-management-frame bodies are irrelevant; only headers are read. | All other frame types and subtypes; frames without a valid FCS, truncated, or malformed (TB10 must supply the validity flag; unflagged frames are ineligible); frames with the retry bit set (over-count guard); frames in Coverage Gap windows, on channels not scheduled, or with no window report; any frame transmitted by the Host Station Identity (the Host's own intentional disconnect or roam is never an attack); any frame from an address of the Monitor Device itself (defensive; the device never transmits, ADR 0001). |

Each eligible frame updates only bounded counters: total, directed-to-Host, broadcast-from-Host-associated-BSS, and a small top-source table (see 2.6). No payloads are retained. Retained addresses follow ADR 0007.

### 2.2 Normalization and minimums (per channel; candidate)

| Item | Value | Guard |
| --- | --- | --- |
| Trailing window | Newest eligible windows of that channel until cumulative `observed_ms` reaches 2000 ms (10 dwells at the default). | Rate is count over Observed Time only. |
| Minimum Observed Time to evaluate | 2000 ms of that channel (evaluation is skipped until the trailing window is full, including after any stale clear or reboot). | One lucky dwell cannot inflate a rate. |
| Eligible count C | Frames in the trailing window. | |
| Rate | `C / Observed Time` in frames per second of Observed Time. | |
| Minimum evidence to open | `C >= 12` and rate `>= 6.0/s`, in at least 3 distinct windows, and no single window holding more than 60% of C. (At 205 ms fully observed windows, 2050 ms, C=13 is the smallest count that reaches 6.0/s.) | Several windows guard against a single burst; the concentration limit guards an AP restart or one mass kick. |
| Recovery threshold (hysteresis) | Trailing rate `< 2.0/s` (one third of open). | Prevents flapping. |
| Required quiet | 30 s of consecutive recovery-clock Observed Time on the episode's channel (default plan: at least 5.6 minutes of wall time WiFi-only; 11 minutes with BLE enabled). | Conservative: recovery is claimed only after a long, fully observed quiet period. A returning anomaly after recovery opens a new episode. |

Episodes are keyed by `(deauth-rate/1, channel)`: at most one per scheduled channel (14 with the widest plan).

### 2.3 Severity (candidate)

| Severity | Condition | Failure mode guarded |
| --- | --- | --- |
| None (Low is not emitted) | Anything below the Medium open condition. | Ordinary roaming and disconnects (1-3 frames per event) never alert. Low is omitted because radio-model starts this rule at Medium. |
| Medium | Open condition in 2.2 is met. | An observed sustained deauth/disassoc rate well above roaming, repeated across dwells. |
| High | Medium open condition met **and** host-targeted count `H >= 3` in the trailing window, with fresh Host context (2.4). | Impact on the Monitored Host requires captured evidence involving it (radio-model, ADR 0003). |

`H` counts eligible frames whose receiver address equals the Host Station Identity, plus broadcast/group frames whose BSSID is the Host-associated BSS. Frames from the Host Station Identity never count (2.1). High is an escalation of an Active Medium episode or a direct open at High when both conditions hold at the opening evaluation.

Host context is fresh when: the Host Station Identity was received in a Host snapshot no older than 15 s of monotonic device time (aligned with the 15 s host-disconnected threshold in [ui-design.md](ui-design.md)); Host WiFi Coverage is observable (2.4 GHz and the episode channel is the Host's channel and in the Channel Plan); and, for the BSS path only, the Host-associated BSS is established by fresh OS-reported BSSID context or fresh frames involving the Host Station Identity. A matching SSID never establishes it.

### 2.4 Confidence (candidate)

| Confidence | Criteria |
| --- | --- |
| low | Episode open (all 2.2 minimums). |
| medium | At least 40 eligible frames in the episode, in at least 5 distinct windows with at least 6 s of episode Observed Time and observed share at least 70%; **or** the same source address counted at least 3 frames on each of at least 2 channels while active (multi-channel repetition). |
| high | Medium criteria **and** Host-directed corroboration, counted cumulatively over the episode: at least 3 frames addressed to the Host Station Identity, or via the fresh Host-associated-BSS path at least 6 qualifying frames (`H` as in 2.3). |

**Caps.** Host Station Identity missing or stale, or Host WiFi Coverage unknown or outside supported coverage (5/6 GHz): Confidence is capped at medium and Severity at Medium; no Host-targeted claim is made and the detail view states why. Association Context unavailable (permission not granted) removes only the BSS path; directed-to-station evidence still works. 5/6 GHz Host activity can be neither observed nor corroborated; a 2.4 GHz BSS with the same SSID does not change that.

### 2.5 Recovery

As in 1.5 with the 2.2 recovery threshold and 30 s requirement. Quiet is per channel. If the Host moves to a different channel mid-episode, the episode continues on its own channel and can only recover on that channel's Observed Time.

### 2.6 Bounded state

Per channel: a ring of at most 12 windows with `{observed_ms u16, count u16, host_count u16}` (about 72 B) plus a top-4 source tally (Misra-Gries, 4 x 10 B) and O(1) episode counters, about 150 B; times at most 14 channels, about 2 KiB total (candidate; M1a RAM budget confirms). Hostile source-address volume cannot grow this: the tally has fixed slots and counts saturate.

### 2.7 Benign scenarios and discriminators

| Scenario | Why no Alert |
| --- | --- |
| Normal roaming | A handful of deauth/disassoc per event; below C and window minimums. Host's own frames are excluded. |
| Intentional Host disconnect, Host sleep/wake | Frames come from the Host Station Identity (excluded), a single directed disassoc, or none. Absence of Host snapshots does not turn into evidence. |
| AP restart or one mass kick | Burst inside one or two windows: the 3-window minimum and 60% concentration limit fail. If it repeats every visit for several visits the rule may legitimately open Medium. |
| Microwave and other interference | Not valid management frames; ineligible. |
| Hidden or sparse channel | Under-sampled channels do not reach the 2000 ms Observed Time; nothing is inferred from silence. |

### 2.8 Fixtures (`deauth-rate/1`)

Common set-up: WiFi-only schedule, channel 6, windows of `planned_ms` 205 and `observed_ms` 200 unless stated, so 10 windows are exactly 2000 ms. "Frames" are eligible frames. `W1..` number windows on channel 6. Host Station Identity known and fresh, Host WiFi Coverage observable, unless stated. Scenario names are proposed file names.

| Scenario name | Set-up | Expected outcome |
| --- | --- | --- |
| `deauth-below-rate` | W1-W10: 11 frames over 4 windows | No episode (5.5/s). |
| `deauth-at-threshold` | W1-W10: 12 frames, 3 in each of W1-W4 | Medium opens at W10, Confidence low. |
| `deauth-above-threshold` | W1-W10: 24 frames over 6 windows | Medium, Confidence low. |
| `deauth-min-observed-not-reached` | W1-W9: 30 frames over 5 windows; nothing more | No episode after W9 (1800 ms). Adding W10 with 0 frames opens Medium. |
| `deauth-min-windows` | W1-W10: 40 frames, 20 in each of two windows | No episode (fewer than 3 windows). |
| `deauth-burst-concentration` | W1-W10: 30 frames, 25 in one window, 5 in another, 0 elsewhere (also fails the 3-window rule) | No episode. Variant with 30 frames over 3 windows of 25/3/2 fails the 60% limit. |
| `deauth-ineligible-frames` | Threshold stream (12 frames) where 3 frames have retry set, 3 are Host-sourced, 3 are beacons | Only eligible frames count; 3 eligible frames, no episode. |
| `deauth-gap-interrupted` | W1-W5: 15 frames (3 each), then 4 gap windows (observed 60 ms, 40 frames each), then W6-W10 with 3 frames each | Gap frames ignored; the gap exceeds 3 revisits so the trailing window clears at resume; after W10 the window holds 1000 ms, no episode; after five more windows of 3 frames (2000 ms, 15 frames, 5 windows) Medium opens. |
| `deauth-recovery-boundary` | Medium open at W10 with all 12 frames in W1-W4 (3 each); then zero-frame windows z1... | Trailing rate falls below 2.0/s at z3 (3 frames, 1.5/s); recovery clock starts at z3. Active at z151 (29.8 s), Ended `observed recovery` at z152 (30.0 s). |
| `deauth-hold-band-resets-clock` | As above; after z102 (20.0 s accrued) insert 10 windows with 8 frames over 4 windows (4.0/s); then zero-frame windows | Episode stays Active, no new Alert, clock reset to zero; ends after a further full 30.0 s of trailing rate below 2.0/s. |
| `deauth-hysteresis-flapping` | Alternate 10 windows at 7/s (window minimums met) with 10 windows at 3/s, for 12 cycles | One Medium episode, never Ended (clock never reaches 30 s), no repeated Alerts. |
| `deauth-gap-pauses-recovery` | Recovery started; after 20.0 s accrued (z102), 20 gap windows (observed 0), then zero-frame windows | Unassessable during the gap, still Active; trailing window refills first (10 windows, no accrual); Active after 59 resumed windows (9.8 s accrued more), Ended at the 60th (30.0 s). |
| `deauth-recovery-clock-discard` | 20.0 s accrued, then no eligible window on the channel for 10 minutes and 1 s, then quiet windows | Accrued time discarded; needs a full 30 s from resume; episode Active meanwhile. |
| `deauth-drop-window-no-quiet` | During recovery, windows with `dropped_events = 1` | Do not accrue; Ended only after 30 s of drop-free eligible windows. |
| `deauth-host-directed-high` | `deauth-above-threshold` (24 frames over 6 windows), 3 of which are addressed to the Host Station Identity (identity fresh, coverage observable) | Severity High, Confidence low (only 24 frames): Severity and Confidence are independent. |
| `deauth-host-directed-high-confidence` | W1-W40 (8.0 s): every third window (W1, W4, ..., W37, 13 windows) carries 4 frames, 52 in total; the 4 frames in W1 and W4 (2 each) are addressed to the Host Station Identity | Opens at W10 (16 frames, 8.0/s) directly at High; by W40 Confidence is high (52 frames, 13 windows, 8.0 s, share 97.6%, 4 directed). |
| `deauth-host-unknown-cap` | As `deauth-host-directed-high` but no Host Station Identity (daemon absent or stale >15 s) | Medium, no Host-targeted claim, Confidence at most medium; stated in detail. |
| `deauth-host-5ghz-cap` | Same frames; Host WiFi Coverage outside supported coverage | Medium, Confidence at most medium. |
| `deauth-ssid-match-only` | Broadcast deauths from a 2.4 GHz BSSID sharing the Host's SSID; Host on 5 GHz, no fresh frames involving the Host Station Identity | Medium; no High. |
| `deauth-multichannel-medium-confidence` | Same source address, 3+ frames on each of channels 1 and 6 while both episodes open | Confidence medium on both episodes (Severity unchanged). |
| `deauth-ack-no-effect` | Acknowledge an Active episode, then run `deauth-recovery-boundary` | Acknowledged flag set; still ends at z152, not before. |
| `deauth-config-change` | Active episode; change dwell to 300 ms or remove channel 6 from the plan | Ended `configuration changed`, no new Alert, trailing evidence cleared; the same frames later reopen only after 2000 ms and the window minimums are met again. |
| `deauth-reboot-interrupt` | Active episode, reboot (new boot identity), same anomaly resumes | Prior episode `interrupted`, no recovery time; a new episode opens only after 2000 ms; new evidence keys carry the new boot identity. |
| `deauth-time-discontinuity` | UTC jumps by +3 h while Active | No state change; recorded annotation; earlier evidence keeps its timestamp and order. |
| `deauth-capacity-pressure` | Store refuses openings while conditions hold on several channels | One per-rule aggregate with suppressed count and peak evidence; no per-channel growth. |
| `deauth-benign-roaming` | Host and AP exchange 2 disassoc plus 2 deauth around a roam; repeated every 5 minutes for 72 h | No episode. |
| `deauth-benign-host-disconnect` | 20 disassociation frames sourced by the Host Station Identity in one minute | No episode (own frames ineligible). |
| `deauth-benign-host-sleep-wake` | Host snapshots stop for 10 minutes, one directed disassoc at sleep, three at wake | No episode; Host context stale, no inference. |
| `deauth-benign-ap-restart` | 30 broadcast deauths in one window followed by 3 in the next | No episode (windows and concentration). |
| `deauth-benign-interference` | Frames with invalid FCS at 100/s | No episode; ineligible. |

### 2.9 Open measurements (M5/M6)

- Loss and revisit: the real fraction of frames received during a dwell, and switch-related loss (feasibility.md shows switching below 1 ms but no loss measurement), to confirm 2000 ms trailing window and the 6.0/s open rate.
- Ambient deauth/disassoc rate per channel in roaming-heavy offices; confirm zero false Medium in the 72 h soak.
- Whether TB10 can deliver an FCS-validity flag and retry bit; if not, the ineligibility rules for those fields are unevaluable and the rule stays disabled until it can.
- Whether `H >= 3` and the 60% concentration limit separate a controlled directed-deauth replay from AP restarts.
- Quiet requirement (30 s) versus real attacker duty; recovery-clock maximum pause (10 min).
- Ring and tally memory in the M1a RAM budget.

## 3. `ble-flood/1` (TB14)

### 3.1 Evidence eligibility

| Eligible | Ineligible |
| --- | --- |
| One event per received legacy LE advertising report: connectable undirected, connectable directed, scannable undirected, non-connectable undirected, in an eligible BLE window. Extended-advertising reports count only if TB11 confirms one event per received PDU. | Scan responses (the device never sends scan requests, ADR 0001); malformed or truncated reports; reports in Coverage Gap windows or with no window report; reports from an address of the Monitor Device itself; anything from Classic Bluetooth. Duplicate handling: **no controller duplicate filtering**. Every received advertisement counts, because repetition is the flood evidence. If TB11 must enable duplicate filtering, this rule is unevaluable and stays disabled with an explicit state, never zero. |

Counts are evidence counts, not distinct devices. Addresses are not physical objects.

### 3.2 Normalization and minimums (candidate)

BLE Observed Time is grouped in **buckets**: a bucket closes when eligible windows have accumulated at least 1000 ms of `observed_ms` (five 200-205 ms windows at the default). Bucket rate is count over that bucket's actual Observed Time. The flood rule evaluates only when a bucket closes. The trailing window is the newest 10 buckets, at least 10 s of Observed Time (about 20 s of wall time with WiFi enabled). Counters are `u32`.

| Item | Value | Guard |
| --- | --- | --- |
| Minimum Observed Time to evaluate | 10 buckets (10 s), including after a stale clear or reboot. | Sparse sampling cannot inflate a rate. |
| Open (Low) | Trailing rate `>= 200/s`, and at least 8 of 10 buckets each `>= 100/s`. Minimum count is 2000 reports in the trailing window. | About 5.5 times the 36/s office figure in feasibility.md; the bucket rule rejects a single spike. |
| Medium | Trailing rate `>= 500/s` and at least 8 of 10 buckets each `>= 250/s`, held at every bucket-close evaluation for 30 consecutive evaluations (30 s of Observed Time, the first being the evaluation that fills the trailing window). Minimum total Observed Time for Medium is therefore about 39 s. | Guards brief bursts (device power-up, reconnect storms) from Medium. |
| Recovery threshold | Trailing rate `< 100/s` (half of Low). | Hysteresis. |
| Required quiet | 60 s of consecutive recovery-clock Observed Time. | Conservative; about 2 minutes wall with WiFi enabled. |

All values are candidates and placeholders: the repo has no measurement of the sustained BLE report rate the controller and queue can deliver, so the thresholds may be unreachable on this hardware. That is safe (under-alerts) and is the first open measurement.

### 3.3 Severity (candidate)

| Severity | Condition | Failure mode guarded |
| --- | --- | --- |
| Low | Open condition. | Sustained rate far above a desk environment. |
| Medium | Medium condition (3.2). | A sustained, not momentary, saturating stream. |
| High, Extreme | Never emitted. | No Host-impact corroboration exists for BLE. |

### 3.4 Confidence (candidate)

| Confidence | Criteria |
| --- | --- |
| low | Episode open. |
| medium | Rate at or above the Low threshold sustained 60 s of Observed Time **and** episode observed share at least 80% **and** at least 3 non-overlapping 10-bucket trailing windows meeting the open condition. |
| high | Never assigned in MVP: there is no corroboration channel (no Host context) that separates a hostile stream from a legitimate dense environment. Confidence is capped at medium. |

Cross-rule coincidence with a churn episode is not used in MVP (rules are independent).

### 3.5 Bounded state

10 buckets of `{observed_ms u16, count u32}` (60 B) plus a top-4 address tally (Misra-Gries, about 12 B each) and O(1) episode counters: under 256 B total, independent of identifier volume (candidate; M1a to confirm). One episode maximum for this rule.

### 3.6 Benign scenarios and discriminators

| Scenario | Why no Alert |
| --- | --- |
| Dense office or airport | Many devices at ordinary intervals: aggregate rate must still exceed 200/s sustained; unproven, see open measurements. M6 records dense environments before defaults are fixed. |
| Ordinary phone/wearable rotation | Changes addresses, not the total report rate; no effect on this rule. |
| Reconnect/power-up burst | Fails the 8 of 10 bucket rule. |
| Microwave, WiFi interference | Reduces reception and produces gaps or malformed reports; ineligible, never evidence. |
| WiFi dwell/BLE scheduling | Rate is per BLE Observed Time, so 50% duty does not change it. |

### 3.7 Fixtures (`ble-flood/1`)

Common set-up: WiFi and BLE both enabled; BLE windows `planned_ms` 205, `observed_ms` 200 (5 windows per bucket, 50 per trailing window). `n/win` is reports per window (so 200 reports per second of Observed Time is 40/win). Windows are numbered B1...

| Scenario name | Set-up | Expected |
| --- | --- | --- |
| `bleflood-below-low` | 39/win for 60 s of Observed Time (195/s) | No episode. |
| `bleflood-at-low` | 40/win from B1 | Low opens at B50 (10.0 s fill, first evaluation); Confidence low. |
| `bleflood-above-low` | 45/win from B1 | Low opens at B50; not Medium (225/s). |
| `bleflood-min-observed-not-reached` | 100/win for B1-B49 (9.8 s) | No episode (bucket 10 not closed). B50 opens Low (500/s). |
| `bleflood-medium-boundary` | 100/win from B1, continuous | Low at B50; Low still active through B194; Medium at B195 (30 evaluations, buckets 10-39). |
| `bleflood-spike-rejected` | 45 windows at 0, then 5 windows at 400/win, then 0 | No episode (only one bucket at or above 100/s). |
| `bleflood-recovery-boundary` | Low open at B50 with 40/win; then 0/win windows z1.. | Ring (evaluated at bucket close) falls below 100/s after 6 zero buckets (80/s); the clock starts with that bucket and accrues one bucket per close. Active through z324, Ended `observed recovery` at z325 (60 buckets, 60.0 s). |
| `bleflood-hold-band` | As above; after 150 quiet windows of the clock, 50 windows at 25/win (125/s) | Remains Active, clock reset, no escalation (125/s is below 200/s), ends after 60 s of trailing below 100/s. |
| `bleflood-hysteresis-flapping` | Alternate 50 windows at 50/win (250/s) with 50 windows at 15/win (75/s), 12 cycles | One Low episode, never Ended. |
| `bleflood-gap-interrupted` | 50 windows at 100/win (Low open), a gap of 10 BLE windows (`observed_ms` 0, 1000/win ignored), then 100/win | Gap reports ignored; the gap exceeds 3 BLE revisit periods so the trailing window clears; Low stays Active and unassessable during the gap; the Medium clock restarts, and evaluation resumes after a 10 s refill. |
| `bleflood-gap-pauses-recovery` | Recovery clock at 30 s, gap, then quiet | Clock paused, resumes; Ended after the remaining 30 s; discarded if pause exceeds 10 min. |
| `bleflood-drops-no-quiet` | Quiet windows with `dropped_events > 0` | Do not accrue. |
| `bleflood-confidence-medium` | 45/win for 60 s of Observed Time at observed share 100% | Severity Low, Confidence medium. Same with observed share 70%: Confidence low. |
| `bleflood-duplicate-filter-unevaluable` | TB11 duplicate filter enabled | Rule state unevaluable; no episode, shown distinctly from quiet. |
| `bleflood-config-change` | Active; toggle BLE off, or change window length | Ended `configuration changed`, no Alert. Toggling WiFi off only reassigns budget: episode kept. |
| `bleflood-reboot-interrupt` | Active, reboot, flood continues | Interrupted; new episode only after 10 s. |
| `bleflood-ack-no-effect`, `bleflood-time-discontinuity`, `bleflood-capacity-pressure` | As the deauth counterparts | Same lifecycle outcomes. |
| `bleflood-benign-dense-office` | 7/win (about 36/s, the recorded office figure) for 72 h simulated | No episode. |
| `bleflood-benign-dense-airport` | Recorded recording, replaying M6 data | Placeholder: fixture must come from M6 recordings; expected: no Medium and at most three Low per 72 h. |
| `bleflood-benign-reconnect-burst` | 5 windows (one bucket) at 200/win, then 7/win for 10 minutes | No episode (one bucket above 100/s; trailing rate stays below 200/s). |

### 3.8 Open measurements (M5/M6)

- Maximum sustained BLE report rate delivered without drops at 50% duty and under UI load (feasibility.md shows zero drops only at about 36/s). If 500/s is unreachable the Medium tier needs a redesign; retain the placeholder until then.
- Dense office, airport, and conference report rates (100/s and 200/s candidates).
- Effect of the controller's scan parameters (50 ms interval, 30 ms window in the VHCI investigation) and WiFi coexistence on observed share.
- Whether extended advertising arrives as one event per PDU.
- Recovery duration (60 s) against realistic hostile flood duty cycles.

## 4. `ble-churn/1` (TB15)

### 4.1 Evidence eligibility

| Eligible | Ineligible |
| --- | --- |
| A sighting of a **random-type** LE address (address type field random: resolvable private, non-resolvable private, or static random) in an eligible BLE window, from an eligible advertising report (same report set as 3.1). | Public addresses (they do not rotate, so a new public address is a new device rather than churn; counted in detail only); scan responses; malformed reports; Coverage Gap windows or unreported windows; the Monitor Device's own addresses; any address in the persisted Transmitter Baselines at first sighting (already known, so not new). |

Identifier changes never claim one physical device changed identity. Payloads are not correlated.

### 4.2 Definitions and bounded state (candidate)

- **Buckets:** N is maintained in 60 buckets of 1 s of BLE Observed Time (a bucket closes when eligible windows accumulate at least 1000 ms of `observed_ms`; five windows at the default). The rule evaluates at bucket close. The trailing window is the newest 60 buckets, so at least 60 s of Observed Time.
- **Known-set:** a fixed table of 256 entries `{address hash u32, first_seen_s u32, last_seen_s u32, reports u8 saturating}` (4 KiB) in Observed Seconds. An entry expires 10 minutes of BLE Observed Time after its last sighting. Its only job is to tell a new address from a repeat.
- **New address:** an eligible address not in the known-set and not in the persisted Transmitter Baselines. Each new address adds 1 to the current bucket.
- **N:** the sum of the trailing 60 buckets (`u16` per bucket, saturating).
- **Warm-up:** after boot, a reset, or any configuration change affecting BLE, sightings are inserted as known and not counted until 300 s of BLE Observed Time. This prevents every boot in a dense place from looking like churn.
- **Table full (256 entries):** a sighting that is not in the table cannot be classified as new or repeat. Two cases, decided when the table fills and re-decided at each bucket close:
  - *Ambient-dominated* (at least 160 entries have `first_seen` older than 60 s of Observed Time): the environment holds more distinct addresses than the rule can track. Opening is suspended, the UI states `identifier volume exceeds tracking`, and untracked sightings are not counted. This is a coverage-like unassessable state, not an Alert, and never becomes evidence. An already active episode continues to be evaluated from its buckets.
  - *Fresh-dominated* (fewer than 160 older entries: the table filled within the trailing window): untracked sightings add 1 to the current bucket, since repeats of addresses first seen seconds ago are far less likely than more new ones in a stream that already filled the table. This can over-count repeats, so it is used only in a stream that is already hot (the Low condition alone needs 100 new addresses).
  No entry is evicted to make room, except by expiry.
- **Baseline admission (obligation on TB11/TB13):** addresses first seen while N is at or above the recovery threshold are counted but not admitted to Transmitter Baselines, so hostile identifier volume cannot evict legitimate baselines.

Bounded state: known-set 4 KiB plus 60 x 2 B buckets plus O(1) episode counters, about 4.2 KiB, independent of identifier volume (candidate; M1a RAM headroom confirms). One episode maximum for this rule.

### 4.3 Minimums, Severity, recovery (candidate)

| Item | Value | Guard |
| --- | --- | --- |
| Minimum Observed Time before any episode | 300 s warm-up plus 60 s for a full trailing window: 360 s of BLE Observed Time from boot, reset, or configuration change. | Sparse sampling and a cold known-set cannot manufacture novelty. |
| Low | `N >= 100` new random addresses in the trailing 60 s (1.67 per second of Observed Time). | The repo has no measured rotation or arrival rate; 100 per minute is a placeholder chosen to sit far above a desk's ordinary rotation (about one address per phone per rotation interval) and to be suspended by table saturation in crowds. |
| Medium | `N >= 200` at the evaluation **and** the Low condition held for 120 consecutive evaluations (120 s of Observed Time, starting at the evaluation that opened the episode). | A sustained stream, not a crowd arriving. |
| Recovery threshold | `N < 40`. | Hysteresis. |
| Required quiet | 120 s of consecutive recovery-clock Observed Time (120 bucket closes). | Ordinary rotation never reaches Low, so a long quiet is cheap. |

### 4.4 Confidence (candidate)

| Confidence | Criteria |
| --- | --- |
| low | Episode open. |
| medium | Low condition met for at least 3 non-overlapping 60 s windows (180 s of Observed Time), **and** at least 80% of the newly counted addresses in the current window have been seen 3 or fewer times (`reports <= 3`; short-lived addresses, unlike rotated addresses that persist), **and** episode observed share at least 80%. |
| cap | Confidence is at most low while observed share is below 50%, and at most medium in all cases. No High: nothing distinguishes churn caused by hostile spam from ordinary privacy-address rotation at high phone density. |

Decoded vendor/type labels are untrusted and are not Confidence inputs in MVP.

### 4.5 Benign scenarios and discriminators

| Scenario | Why no Alert |
| --- | --- |
| Ordinary phones/wearables rotating (about every 15 minutes) | Low new-address rate; addresses persist (`reports` above 3); warm-up covers boot. |
| Crowd arrival, dense airport, office | Distinct addresses fill the known-set with ambient entries: opening is suspended and visible (`identifier volume exceeds tracking`), never inferred; known devices are not re-counted. |
| Public-address devices coming and going | Public addresses are ineligible. |
| Sparse coverage | Fewer sightings means a lower N (under-alert), plus a 300 s warm-up and the observed-share cap. |
| Boot in a busy place | Warm-up. |

### 4.6 Fixtures (`ble-churn/1`)

Common set-up: WiFi and BLE enabled, BLE windows `observed_ms` 200 (300 windows per 60 s). Warm-up already complete unless stated. "Arrival at window w" means one new random address first sighted in window w. A 60 s trailing window is 300 windows.

| Scenario name | Set-up | Expected |
| --- | --- | --- |
| `blechurn-below-low` | 99 arrivals within a 60 s span (arrivals at windows 3, 6, ..., 297), then none | Peak N 99; no episode. |
| `blechurn-at-low` | Arrivals at windows 3, 6, ..., 300 (100 in exactly 60 s) | Low opens when bucket 60 closes (window 300); Confidence low. |
| `blechurn-above-low` | 150 arrivals evenly over 300 windows | Low; not Medium. |
| `blechurn-medium-n-below` | Steady 199 arrivals in every 60 s, evenly by bucket, held 10 minutes | Low opens at bucket 60; stays Low (N 199 is below 200). |
| `blechurn-medium-boundary` | Steady 200 arrivals in every 60 s, evenly by bucket | Low opens at bucket 60; Low active through bucket 178; Medium at bucket 179 (120 evaluations, buckets 60-179, window 895). |
| `blechurn-warmup-not-elapsed` | 500 arrivals in the first 200 s after boot | No episode; nothing was counted (warm-up inserts as known). Arrivals after 300 s count normally. |
| `blechurn-min-observed-not-reached` | After warm-up, 100 arrivals by window 299 (the 60th bucket not closed) | No episode at window 299; opens at bucket 60 (window 300) if N is still at least 100. |
| `blechurn-recovery-boundary` | `blechurn-at-low`, then no arrivals | N falls below 40 at bucket 97 (trailing sum 39); the clock starts with that bucket; Active at bucket 215, Ended `observed recovery` at bucket 216 (120 buckets). |
| `blechurn-hold-band` | As above; after 60 quiet buckets, arrivals resume at 50 per 60 s | Remains Active, clock reset, no escalation; ends after a further 120 consecutive buckets below 40. |
| `blechurn-hysteresis-flapping` | Alternate 60 s at 110 arrivals with 60 s at 30, 12 cycles | One Low episode, never Ended (N does not stay below 40 for 120 buckets). |
| `blechurn-gap-interrupted` | Arrivals, then a gap spanning more than 3 BLE revisit periods, then arrivals | Trailing buckets cleared on stale; Low stays Active and unassessable during the gap; recovery clock paused (discarded after a 10 min pause). |
| `blechurn-table-full-ambient` | After warm-up, 300 distinct random addresses each re-sighted repeatedly over 10 minutes (no arrival rate above 1 per 2 s) | Opening suspended with `identifier volume exceeds tracking`; no episode; memory constant at 4 KiB. |
| `blechurn-table-full-fresh` | After warm-up, 5 new addresses per window continuously | Table fills within 60 s with fresh entries; untracked sightings count as new; Low opens at bucket 60; memory constant at 4 KiB. |
| `blechurn-baseline-admission` | Hostile churn N 200 with 512 full Baselines | No Baseline is evicted by churn-period addresses. |
| `blechurn-public-addresses-ignored` | 300 distinct public addresses in 60 s | No episode. |
| `blechurn-confidence-medium` | Three consecutive 60 s spans at N 120, 90% of new addresses with 3 or fewer reports, observed share 100% | Severity Low, Confidence medium. With only 50% short-lived: Confidence low. |
| `blechurn-config-change` | Active; toggle BLE off or change the window | Ended `configuration changed`; known-set cleared and warm-up restarts. |
| `blechurn-reboot-interrupt` | Active, reboot, churn continues | Interrupted; the new episode requires 360 s of Observed Time (warm-up plus window). |
| `blechurn-ack-no-effect`, `blechurn-time-discontinuity`, `blechurn-capacity-pressure` | As the deauth counterparts | Same lifecycle outcomes. |
| `blechurn-benign-phone-rotation` | 50 phones each rotating every 15 min for 72 h, addresses persisting (reports above 3) | No episode. |
| `blechurn-benign-dense-airport` | M6 recording | Placeholder: expected no Medium and at most three Low per 72 h; fixture depends on M6 data. |
| `blechurn-benign-boot-in-crowd` | 200 addresses within 60 s of boot | Warm-up: no episode. |

### 4.7 Open measurements (M5/M6)

- Real rotation rates and address-lifetime reports in offices, homes, and airports (confirms 100 and 200 per 60 s, and whether N 100 stays below dense-crowd novelty).
- Reports per address at 50% duty (the `reports <= 3` singleton criterion).
- Whether 256 known-set entries and the 160-entry ambient limit are large enough for an ordinary office (otherwise the rule is suspended too often); RAM headroom from M1a.
- Whether decoding address type (random subtype) is dependable from TB11.
- Warm-up length (300 s) against realistic boots, and Baseline admission policy with TB13.
- Recovery duration (120 s).

## 5. Cross-rule validation gates

These come from [mvp-validation.md](mvp-validation.md) and ADR 0015; they are gates, not thresholds.

- 72 h ordinary-desk soak: zero known-false Medium or High Alerts and at most three known-false Low per rule. `deauth-rate/1` has no Low, so any false deauth Alert is a Medium and fails the gate.
- Controlled replay of every fixture above triggers the rule at its boundary and exercises recovery, Coverage Gaps, acknowledgment, and reboot interruption; the same shared C++ rule code runs on device and in replay.
- No fixture emits Extreme or High from a BLE rule.
- Any change to a number in this document after M5/M6 is a version bump and a matching fixture change, made before the soak that uses it.
