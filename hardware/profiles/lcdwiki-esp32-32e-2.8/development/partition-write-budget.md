<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Provisional partition, record, and endurance budget

F08's last open item ahead of C01/TB02 is the partition/write/endurance
budget [ADR 0017](../../../../docs/adr/0017-use-a-recoverable-flash-journal.md)
and M1a's exit criteria call for. M6 owns the real journal
implementation (schema, reclamation algorithm, migration); this is a
narrower, bounded pass that costs [radio-model.md](../../../../docs/radio-model.md)'s
32 active / 128 ended-or-interrupted Alert and 512 Transmitter Baseline
targets in bytes, checks the result against the `state` partition
`firmware/partitions.csv` already reserves, and grounds a write-endurance
estimate in a real measurement (`DEV:PARTITION_WRITE_TEST`) rather than an
assumption. No wire format, on-flash schema, or reclamation algorithm is
fixed here.

## Partition layout as built (not as intended)

Building this evidence surfaced a local environment bug worth recording:
`firmware/sdkconfig` (gitignored, generated once and then reused) had been
generated before `sdkconfig.defaults` was updated to select the custom
4 MB partition table, and idf.py only applies `sdkconfig.defaults` to a
*fresh* sdkconfig -- it silently kept building against ESP-IDF's built-in
single-app default (1 MB `factory`, no `state` partition) instead.
`DEV:PARTITION_WRITE_TEST`'s first run correctly reported the `state`
partition as not found. Deleting `firmware/sdkconfig` and `firmware/build`
and rebuilding fixed it; `esptool.py`'s read-back of the flashed partition
table now matches `partitions.csv`. This was a stale local build artifact,
not a committed bug -- `firmware/sdkconfig` isn't tracked -- but it's worth
flagging because the same staleness could silently affect anyone's plain
`./dev build firmware`/`hardware` after a future `sdkconfig.defaults`
change, unless they know to delete the cached file.

## Record byte budget

Two fixed-size record kinds, matching ADR 0017's "settings/calibration
stay in NVS; Alerts and Transmitter Baselines use a dedicated checksummed
append journal" split. Fixed-size records (not variable-length/CBOR-style)
are deliberate: a journal that must detect and skip torn writes after
sudden power loss is far simpler to validate at a known, constant offset
stride than one that must first parse a length field that could itself be
the torn part.

**Alert record (detailed active/ended/interrupted), provisional 128 bytes:**

| Field | Bytes | Notes |
| --- | ---: | --- |
| record_type, schema_version | 2 | journal entry kind + migration version |
| episode_id, boot_identity_id | 8 | durable ordering across boot identities (M2) |
| rule_id, severity, peak_severity, confidence | 4 | confidence unused/sentinel for Operational Alerts |
| state, acknowledged, end_reason | 3 | active/ended/interrupted/config-changed |
| first/last evidence: device ms, UTC ms, uncertainty ms | 36 | 2x (u64 + u64 + u32); UTC 0 when unavailable |
| observed_time_accumulated_ms, evidence_count | 8 | coverage-gap-aware Observed Time, per radio-model.md |
| host_targeted, coverage_gap_active_at_close | 2 | |
| primary transmitter/BSSID address | 6 | raw over-the-air address, not text |
| secondary transmitter count + up to 4 addresses | 25 | bounded top offenders for churn/flood context |
| operational_subsystem, operational_cause_code | 3 | 0 for Radio Alerts |
| firmware_version_id, rules_version_id | 4 | provenance |
| CRC32 | 4 | per-record integrity, so a torn/corrupt entry is individually detectable and skippable, not just at journal granularity |
| **subtotal / reserved** | **105 / 23** | rounded up to 128 for alignment and forward-compatible fields |

**Transmitter Baseline record, provisional 96 bytes:**

| Field | Bytes | Notes |
| --- | ---: | --- |
| address, address_type, transmitter_kind | 8 | raw address + BSSID/BLE-public/random/resolvable + WiFi-AP/BLE-peripheral |
| first_seen_ms, last_seen_ms, last_seen_utc_ms | 24 | |
| observed_count | 4 | |
| smoothed / current Signal Strength | 4 | i16 each |
| ssid_len + up to 32 bytes of SSID/common-beacon label | 33 | bounded Radio Label per radio-model.md; escaped at render, not here |
| lru_rank/last_access_seq, flags | 5 | LRU eviction ordering for the 512-entry cap |
| CRC32 | 4 | |
| **subtotal / reserved** | **82 / 14** | rounded up to 96 |

## Working set vs. the reserved partition

| Record kind | Count | Size | Bytes |
| --- | ---: | ---: | ---: |
| Active detailed Alerts | 32 | 128 B | 4,096 |
| Ended/interrupted Alerts | 128 | 128 B | 16,384 |
| Transmitter Baselines | 512 | 96 B | 49,152 |
| **Working set total** | | | **69,632 (~68 KiB)** |

An append-only journal needs free space beyond the live working set to
keep writing while old, superseded entries (an Alert that moved from
active to ended, an evicted LRU Baseline) are still physically present
awaiting reclamation. Without M6's actual reclamation algorithm to size
against, this note uses a deliberately simple, conservative placeholder:
**2x the working set**, i.e. enough free space to write a whole second
copy of everything live before reclamation must run:

- Working set: 69,632 B
- Reclamation headroom (2x): 139,264 B
- **Provisional journal region: 208,896 B (~204 KiB)**

`partitions.csv` already reserves 0x1F0000 (2,031,616 B, ~1.94 MiB) for
the `state` partition -- **about 9.7x this provisional region**. That
partition size was fixed before this budget existed, not derived from it;
the finding here is that it is comfortably, not marginally, sufficient
for the 32/128/512 targets even with a generous 2x reclamation multiplier,
and has roughly an order of magnitude of margin for those targets to grow
before the partition itself becomes the constraint. Shrinking `state` to
free flash for other use, or leaving the margin for count growth, is an
M6 product decision this note does not make.

[radio-model.md](../../../../docs/radio-model.md) additionally asks to
"reserve space for Operational and aggregate Capacity-pressure Alerts so
detailed matches cannot crowd out failure reporting." Given the partition
headroom above, the simplest approach is a **reserved sub-quota within the
32 active-Alert slots** (e.g. a minimum of 4 always kept free for
Operational/Capacity-pressure use) rather than a separate byte pool --
recorded here as a recommendation for M6 to accept or revise, not a
decision this note is positioned to make.

## Write/erase cost: measured, not assumed

`DEV:PARTITION_WRITE_TEST` (new `firmware/platform/esp32/dev_console/dev_console.cpp`
command) erases one 4,096-byte sector of the (currently unused) `state`
partition via `esp_partition_erase_range`, then writes twenty 128-byte
records via `esp_partition_write` at increasing offsets within it, timing
each call with `esp_timer_get_time()`, then re-erases the sector so no
test pattern is left for whatever reads this partition first. Run live on
the E32R28T on `/dev/cu.usbserial-140`, idle otherwise, after the
partition-table fix above:

| Metric | Value |
| --- | ---: |
| 4,096 B sector erase | 2,850 μs |
| 128 B record write (min / avg / max, n=20) | 701 / 716 / 861 μs |
| Write failures | 0 |

This is close to (and slightly faster than) `RunNvsWriteTest`'s
previously-measured ~775 μs average NVS blob write (see
[feasibility.md](feasibility.md#cpu-load-and-nvs-write-rate-under-combined-load)) --
consistent with NVS itself ultimately calling the same
`esp_partition_write` primitive plus its own wear-leveling/metadata
bookkeeping on top. The board stayed fully responsive afterward
(`DEV:CAPACITY_STATUS` and `DEV:PERIPHERAL_STATUS` both normal, MicroSD
still mounted).

## Endurance: a bounded estimate, not a datasheet-verified figure

The provisional journal region above is ~204 KiB, i.e. 51 four-KiB
sectors. Using the measured 2,850 μs/sector erase cost and a deliberately
pessimistic assumption -- the entire working set fully turns over and
triggers one complete reclamation pass (51 sector erases) **once per
day** under continuous busy conditions, which is far more churn than 32
active Alerts and 512 slowly-evolving Baselines are likely to see in
ordinary use:

- 51 sector erases/day / 51 sectors = 1 erase-cycle/sector/day.
- A commonly-quoted minimum endurance figure for commodity SPI NOR flash
  parts (the kind these ESP32 dev boards typically carry) is **>=100,000
  erase cycles/sector** -- this is a widely-used assumption, explicitly
  **not verified against this specific board's exact flash chip
  datasheet**, which this note does not have on hand.
- At 1 erase/sector/day: ~100,000 days, ~274 years to exhaust a sector.
- Even at a 10x more pessimistic 10 full reclamations/day: ~10,000 days,
  ~27 years.

Either reading leaves a comfortable margin against any realistic MVP
service life. This conclusion is only as good as its two stated
assumptions (daily full-working-set turnover, and >=100,000-cycle commodity
NOR endurance) and does not model wear-leveling write amplification from
whatever reclamation algorithm M6 actually builds, or the separate `nvs`
partition's own settings/calibration write endurance (unaffected by this
journal; `RunNvsWriteTest`'s ~775 μs figure is the relevant one there).
Actual per-sector cycle counts for this board's flash chip and a
concrete reclamation cadence remain M6 work.

## What this does and does not close

This grounds F08's remaining partition/write/endurance item in a
measured per-record write cost, a measured sector-erase cost, and a
documented (not just asserted) record-size and journal-region budget that
fits comfortably inside the already-reserved `state` partition. It does
**not** implement the journal, decide its reclamation algorithm, or fix
the on-flash record schema -- those are M6's. It also does not exercise
partition writes under the combined radio/touch/flash load the rest of
this record measures separately; that remains open alongside F08's other
still-open combined-load item.
