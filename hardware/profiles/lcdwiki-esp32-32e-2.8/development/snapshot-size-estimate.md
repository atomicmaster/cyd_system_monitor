<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# One-second Host Metric snapshot: a pre-C01 size estimate

F08's serial-headroom check has so far used an arbitrary 1,024-byte
"representative snapshot" placeholder (see
[feasibility.md](feasibility.md#accounting-rules-already-fixed)). C01
defines the real CBOR schema, golden vectors, and codecs; this note is a
narrower, bounded pass ahead of that ticket: walk [host-metrics.md](../../../../docs/host-metrics.md)'s
field list, [ADR 0011](../../../../docs/adr/0011-use-versioned-cbor-over-usb.md)'s envelope, and
[device-lifecycle.md](../../../../docs/device-lifecycle.md)'s negotiation
description field-by-field in CBOR (RFC 8949) to get a size number with a
documented basis, so [`serial_snapshot_probe.py`](serial_snapshot_probe.py)
sends something more defensible than a round number. It does not replace
C01: no wire format is fixed here, field order is not decided, and nothing
here is generated or golden.

## Method

CBOR items cost: 1 byte for a map/array header up to 23 entries, 1 byte
for an unsigned int 0-23 (2-9 bytes above that), 5 bytes for a packed
float32, 9 for a float64/u64, and `1 + length` bytes for a short text or
byte string (length < 24; 2 + length up to 255). Map keys are assumed to be
small integers (schema-assigned field IDs), not field-name strings --
integer keys are the cheap choice C01 should make; this estimate uses them
throughout rather than assuming the more expensive alternative.

## Envelope (ADR 0011: version, type, sequence, timestamp, integrity)

| Field | CBOR encoding | Bytes |
| --- | --- | ---: |
| Outer map header (6 entries) | map(6) | 1 |
| Protocol version | key(1) + uint | 2 |
| Message type | key(1) + uint | 2 |
| Sequence number (u32) | key(1) + uint32 | 6 |
| Timestamp (u64 ms epoch) | key(1) + uint64 | 10 |
| Integrity check (CRC32) | key(1) + bstr(4) | 6 |
| Body | key(1) + `<body>` | 1 + body |
| **Envelope fixed cost** | | **27** |

Plus a 4-byte length prefix outside the CBOR item itself (framing, not
CBOR) per ADR 0011's "length-prefixed CBOR messages."

## Snapshot body, per host-metrics.md row

| Host Metric | Encoding notes | Bytes |
| --- | --- | ---: |
| CPU use | availability + f32 + age(u16) | 14 |
| Physical RAM Occupied | availability + 2x u64 (used/total) + age | 28 |
| Root disk capacity | availability + 2x u64 + age | 28 |
| Root disk read/write rate | availability + 2x f32 + age | 20 |
| Interfaces and addresses | see below | 630 |
| Network RX/TX rate | availability + 2x f32 per interface, 8 interfaces | 122 |
| Host Station Identity | availability + MAC as **text** (17 chars) + private flag + age | 28 |
| Host WiFi band/channel | availability + 2x uint + age | 12 |
| Current WiFi BSS | availability + SSID(<=32B text) + BSSID(text) + age | 60 |
| Battery charge/state | availability + uint + enum + age | 12 |
| Battery condition | availability + uint + enum + age | 12 |
| Agent Activity (3 agents) | availability + enum + bool, x3 | 21 |
| Body map header (12 entries) | map(12) | 1 |
| **Body total** | | **~988** |

**Total representative snapshot: ~1,015 CBOR bytes + 4-byte length prefix
= ~1,019 bytes on the wire.**

### Interfaces and addresses: the dominant, and least bounded, cost

This one field is 62% of the body. `getifaddrs`-based interface
enumeration on a real Mac commonly returns `en0`, `en1`/`en3` (Ethernet
dongle), `awdl0`, `llw0`, and `utun0`-`utun3` (VPN/Continuity) even with no
extra network hardware -- 8 non-loopback interfaces is a realistic
midpoint, not a pessimistic outlier, and machines with `bridge100`
(Internet Sharing) or more `utun` peers than shown here go higher. The
estimate above assumes:

- **8 interfaces** (the working assumption, not yet a schema limit).
- **Up to 2 addresses each** (one IPv4, one IPv6), text-encoded: a decimal
  IPv4 string is up to 15 characters, a full IPv6 string up to 45.
- Interface name up to 6 characters (`utun10` is 6; this undercounts
  nothing in the observed set above).

None of this is bounded in the schema today -- C01's Work item 1
("Decide limits from F08 evidence rather than unbounded containers")
still needs to pick real caps. Two concrete, cheap changes for C01 to
consider, since they cost nothing today and shrink the dominant field
substantially:

1. **Encode addresses and MAC/BSSID as raw byte strings, not text.** An
   IPv4 address is 4 raw bytes (5 with header) instead of a 16-byte text
   string; IPv6 is 16 raw bytes (17 with header) instead of 46; a 6-byte
   MAC is 7 bytes instead of 18. Applied to every address/MAC/BSSID field
   above, the body total drops to roughly **560 bytes** (snapshot ~590
   bytes on the wire) -- essentially free, since the daemon already holds
   these as raw bytes before formatting them for display.
2. **Cap reported interfaces explicitly** (e.g. 8, with a truncation flag
   for the rest) so a host with an unusual number of `utun` peers cannot
   make a "complete" snapshot silently grow past whatever this test
   validates. An unbounded array is also the kind of untrusted-input
   surface [`README.md`](../../../../README.md)'s working rules ask every
   USB message to treat as bounded.

## What this changes about F08's serial check

The prior 1,024-byte placeholder turns out to be a reasonable midpoint
for the **text-encoded, 8-interface** case (~1,019 B), not an arbitrary
round number -- but it is not a validated upper bound, since interface
count and address encoding are both still undecided. `serial_snapshot_probe.py`
sends a configurable payload size so the same tool can check both this
estimate and the ~590-byte binary-encoded alternative once C01 picks one.
This note's numbers are a sizing input to C01, not a substitute for it;
C01 still owns the real field list, order, and any fields (Host WiFi
context detail, more Agent fields, etc.) this pass may have
under- or overestimated.
