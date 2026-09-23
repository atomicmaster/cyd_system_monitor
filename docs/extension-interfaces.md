<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Extension interfaces for feature slices

What a feature ticket edits, what it must not, and the contracts it can rely
on. Proven by the CPU path plus RAM as a stub metric (C02), Storage (TB04), Battery (TB06), Agents (TB07), and Network (TB05); the radio,
episode, configuration, and persistence interfaces below are written
contracts only and are refined by TB09, TB13, and TB16.

## Adding a metric (proven: CPU, RAM, Storage, Battery, Agents)

A metric is a *group* in the complete snapshot, one collector on the Host, and
one card on the display. Each step has exclusive files; the only shared edit is
one line in one list.

| Step | Owner edits | Shared edit |
| --- | --- | --- |
| Wire group | `protocol/CORE_PROTOCOL.md` group table row; `protocol/core/cpp/protocol.{hpp,cpp}` and `host/protocol/src/lib.rs` group types and codec; fixtures in `protocol/tools/gen_vectors.py` + `parity-matrix.txt` rows; group key = the next free body key | protocol files are serial: one metric at a time, gated by `parity_diff.py` |
| Device state | a field on `HostState` and its apply/advance in `firmware/domain/src/host_state.cpp` | serial with the wire step |
| Card | `firmware/domain/src/cards/<id>_card.cpp` (`Make<Id>Card`) | one `CYD_CARD(id, MakeIdCard)` line in `firmware/domain/include/firmware/domain/card_registry.def` |
| Host collector | a `metrics::Collector` impl under `host/platform-macos/src/<id>.rs`; wire it into the snapshot in `host/cli/src/link.rs` | `link.rs` collector list (one line) |
| Scenario | fixtures/expectations under `simulator/scenarios/<id>-*.expected` and a scenario file | one scenario registration in `simulator/src/main.cpp` |

New card files are found by a CMake glob; re-run `./dev build` (or touch
`firmware/domain/CMakeLists.txt`) after adding one. ESP-IDF's early scan runs
that file in script mode, so `CONFIGURE_DEPENDS` cannot be used.

Rules every group follows:

- **Additive, all-or-nothing:** a group is a nested map at body key ≥ 5; when
  present every field it defines is required; unknown keys inside are skipped
  under the shared bounds; appearing twice is malformed. Protocol stays 1.x.
- **Availability, freshness, and enablement stay separate.** A missing value is
  `--` plus a note, never `0`; stale is retained and dimmed with `Last`.
- **Byte counts, not percentages,** on the wire when the source is a count; the
  card chooses the presentation.
- **Freshness is per group:** each group is stale 3 s after it was last
  received (a group with its own slower source, like Storage capacity, carries a
  source age instead and the device derives freshness from it; the source time
  is signed, so a sample older than the device's uptime is not clamped to look fresh), independently of the CPU/link state (RAM stops being sent → RAM goes
  stale while CPU stays live).
- **Fixture first:** a new edge case gets a fixture and a matrix row before the
  fix, and `./dev check protocol|host|firmware` runs the C++/Rust parity gate.

### Bounds (measured)

- A CPU snapshot with UTC is 61 bytes worst case; the RAM group adds at most 33
  (map key, five fields with 64-bit counts); the Storage group adds at most 58
  (eight fields, four 64-bit and two 32-bit values); the Battery group adds at most 38
  (ten fields, four 16-bit values, four one-byte enums, two 32-bit ages); the Agents group adds at most 16
  (five fields: availability, one 32-bit age, three one-byte agent states); the Network group adds at most 98 (thirteen fields: two 64-bit rates, two 32-bit ages, a 32-byte SSID, two MAC addresses), over the 64-byte budget and covered by a maximum-size fixture. The snapshot body accepts up to 10 keys. Frame limit is 2,048; the link
  carries about 11 KB/s at 115,200 baud, so one complete snapshot per second
  uses well under 1%.
- Budget for the six MVP cards: up to 64 bytes per group, so all groups plus the
  envelope stay under 500 bytes, a quarter of the frame limit. A group that
  needs more (per-interface network rows, agent lists) states its own maximum
  in its ticket and is checked against the 2,048 limit by a maximum-size
  fixture.

### Ownership and threading

- `HostLink`/`HostFeed`/`HostState` run on the app loop only; the UART reader
  task only queues bytes (`platform/esp32/usb`). LVGL is driven from the same
  loop. Cards are pure functions of `HostState` and the monotonic clock.
- Host collectors are called once per second from the session loop with a
  monotonic millisecond clock. They never open ports and never block on a slow
  source: a slow source keeps its own cache and returns the last sample with its
  age, or an unavailable sample (Battery condition runs `system_profiler` on a
  background thread with a hard timeout and output cap, and `collect` only reads
  the cache; Agents runs a bounded libproc scan, about 6-17 ms, inline every 5 s and reuses it in between).
- Ordering evidence uses boot identity plus monotonic sequence; UTC is display
  context only.

## Written contracts (not yet implemented)

These fix the shape so TB09, TB13, and TB16 can refine independently; no numeric
threshold is committed here.

- **Radio observation (TB09/TB10/TB11):** an observer produces bounded
  *observation events* (protocol, channel or BLE window, timestamp in monotonic
  ms plus boot identity, minimal metadata) into a fixed-size queue and a
  separate *coverage record* per window (dwell, gap, dropped count). Overload
  drops oldest events and increments a dropped counter that is itself
  reported; rule code consumes events only through this interface and lives in
  shared C++ (`firmware/domain/`), never in Rust.
- **Episode evidence (TB12–TB15):** an episode keeps its ordered evidence keys
  (boot identity, sequence), first/last evidence order, acknowledged flag, and
  state Active/Ended/Interrupted. Ended only by observed-quiet; a reboot marks
  Active episodes Interrupted without inventing an end time. Minimum evidence,
  Confidence, and quiet-recovery semantics are documented by each rule owner
  before its scenario expectations are fixed.
- **Configuration revision (TB16):** `configuration_revision` is a monotonic
  `u32` owned by the device. A settings change applies once, idempotent by
  command identity, and increments the revision; a write naming an older
  revision is rejected as stale (`rejected`), never merged. The revision is
  carried in Hello and in every snapshot so the Host detects divergence.
- **Persistence adapter (TB13):** an interface with `Append(record)` returning
  only after commit, `Recover()` yielding committed records in order while
  skipping and counting torn or corrupt entries, and `Reclaim()`; settings and
  calibration stay in NVS. ADR 0017 sets the journal design; record sizes and
  reclamation are fixed by TB13 before implementation.
