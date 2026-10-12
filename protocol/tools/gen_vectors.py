#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write the hand-specified compatibility/malformed vectors.

Independent of both codecs: bytes are assembled from the frame rules in
CORE_PROTOCOL.md (u16-be length | canonical CBOR | CRC-32-be over the CBOR).
The golden vectors are not regenerated here.
"""
import pathlib
import zlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "fixtures"
SESSION = bytes(range(16))


def uint(major, v):
    if v < 24:
        return bytes([major << 5 | v])
    for arg, n in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if v < 1 << (8 * n):
            return bytes([major << 5 | arg]) + v.to_bytes(n, "big")


def frame(cbor, length=None, crc=None):
    n = len(cbor) + 4 if length is None else length
    c = zlib.crc32(cbor) if crc is None else crc
    return n.to_bytes(2, "big") + cbor + c.to_bytes(4, "big")


def kv(items):
    return b"".join(uint(0, k) + v for k, v in items)


def u(v):
    return uint(0, v)


def envelope(kind, body, extra=(), major=1, seq=1, keys=None):
    items = keys or sorted(
        [(0, u(major)), (1, u(0)), (2, u(kind)), (3, uint(2, 16) + SESSION),
         (4, u(seq)), (5, body)] + list(extra))
    return uint(5, len(items)) + kv(items)


CPU = uint(5, 5) + kv([(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4))])
HELLO = uint(5, 4) + kv([(0, u(1)), (1, u(0)), (2, u(0x21)), (3, u(9))])
nest = lambda d: b"\x81" * d + b"\x00"


def write(name, data, note):
    (OUT / name).write_text(f"# SPDX-License-Identifier: CC0-1.0\n# {note}\n{data.hex()}\n")


snap = frame(envelope(4, CPU))
write("cpu-snapshot-v1-unknown-nested.hex",
      frame(envelope(4, CPU, [(9, uint(4, 2) + uint(4, 2) + u(1) + u(2) + uint(4, 2) + u(3) + u(4))])),
      "unknown optional key 9 holding [[1,2],[3,4]]: skipped, snapshot applies")
write("cpu-snapshot-v1-large-unknown.hex",
      frame(envelope(4, CPU, [(9, uint(2, 1100) + bytes(1100))])),
      "unknown optional key 9 holding 1,100 bytes: a ~1.2 KB frame, over the old 1,024 cap")
write("cpu-snapshot-v1-too-deep.hex",
      frame(envelope(4, CPU, [(9, nest(4))])),
      "unknown optional key 9 nested four arrays deep: over the skip limit")
write("cpu-snapshot-v1-tag.hex",
      frame(envelope(4, CPU, [(9, b"\xc0\x00")])),
      "unknown optional key 9 holding a CBOR tag: outside the profile")
write("cpu-snapshot-v1-duplicate-key.hex",
      frame(envelope(4, CPU, keys=[(0, u(1)), (0, u(1)), (1, u(0)), (2, u(4)),
                                   (3, uint(2, 16) + SESSION), (4, u(1)), (5, CPU)])),
      "envelope key 0 present twice")
write("cpu-snapshot-v1-missing-body.hex",
      frame(envelope(4, b"", keys=[(0, u(1)), (1, u(0)), (2, u(4)),
                                   (3, uint(2, 16) + SESSION), (4, u(1))])),
      "envelope without required key 5")
write("cpu-snapshot-v1-zero-length.hex", frame(b"\x00" * 5, length=0),
      "declared length 0 followed by bytes")
write("cpu-snapshot-v1-trailing-byte.hex", snap + b"\x00",
      "one byte beyond the declared length")
write("cpu-snapshot-v1-truncated.hex", snap[:-1], "final CRC byte missing")
write("cpu-snapshot-v1-oversized-length.hex", frame(envelope(4, CPU), length=2048),
      "declared length 2048 exceeds the 2046-byte limit")
write("hello-reply-v1.hex", frame(envelope(2, HELLO)), "HelloReply, minor 0")
write("hello-v1-incompatible-major.hex", frame(envelope(1, HELLO, major=2)),
      "Hello from major 2: reply with Error, apply nothing")
write("hello-v1-wrong-body.hex", frame(envelope(1, uint(4, 1) + u(1))),
      "Hello whose body is an array, not a map")
write("error-v1.hex", frame(envelope(7, uint(2, 3) + b"\x01\x02\x03")),
      "Error carrying a 3-byte opaque diagnostic")

# Found by protocol/tools/parity_diff.py (differential gate): each was a
# C++/Rust disagreement or an unguarded rule, written down before the fix.
cbor = body_of_env = envelope(4, CPU)
write("cpu-snapshot-v1-trailing-envelope-bytes.hex", frame(cbor + b"\x00"),
      "bytes remain after the envelope map: malformed, not a version error")
write("cpu-decoder-hello-kind.hex", frame(envelope(1, CPU)),
      "type Hello carrying a snapshot-shaped body, fed to the snapshot decoder")
write("snapshot-v2-unknown-body.hex",
      frame(envelope(4, uint(3, 6) + b"future", major=2)),
      "major 2 with a body layout this decoder cannot know: unsupported-major, body never read")
write("hello-v2-unknown-body.hex",
      frame(envelope(1, uint(3, 6) + b"future", major=2)),
      "major 2 Hello with an unknown body layout: unsupported-major, body never read")
write("error-v1-oversized-body.hex", frame(envelope(7, uint(2, 600) + bytes(600))),
      "Error with a 600-byte body exceeds the 512-byte non-snapshot limit: too-large")

write("cpu-snapshot-v1-duplicate-major.hex",
      frame(envelope(4, CPU, keys=[(0, u(1)), (0, u(2)), (1, u(0)), (2, u(4)),
                                   (3, uint(2, 16) + SESSION), (4, u(1)), (5, CPU)])),
      "envelope key 0 repeated with a different major: malformed key order beats the version error")
CPU_UNKNOWN = uint(5, 6) + kv([(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4)),
                               (15, uint(4, 2) + u(1) + uint(3, 1) + b"x")])
write("cpu-snapshot-v1-unknown-body-field.hex", frame(envelope(4, CPU_UNKNOWN)),
      "unknown optional snapshot-body field 15 holding [1, \"x\"]: skipped like any bounded value")
write("error-v1-kind-unknown.hex",
      frame(envelope(9, uint(2, 600) + bytes(600))),
      "unknown message type 9 with an oversized body: malformed type, not a size error")

write("cpu-snapshot-v1-bytes-body.hex", frame(envelope(4, uint(2, 3) + b"\x01\x02\x03")),
      "Snapshot whose body is a byte string, not a field map: malformed, never a CPU-less snapshot")

# Control bodies skip unknown optional keys like every other map; nesting depth
# is counted from the unknown value itself, wherever it sits.
PAIR = uint(5, 3) + kv([(0, uint(2, 16) + bytes([0x11] * 16)),
                        (1, uint(2, 16) + bytes([0x22] * 16)), (2, u(123456))])
def with_unknown(body, key, value):
    return bytes([body[0] + 1]) + body[1:] + uint(0, key) + value
ARRAY = uint(4, 2) + u(1) + uint(3, 1) + b"x"
write("hello-v1-unknown-body-field.hex",
      frame(envelope(1, with_unknown(HELLO, 9, ARRAY))),
      "Hello with unknown optional body key 9 holding an array: skipped")
write("pair-v1-unknown-body-field.hex",
      frame(envelope(3, with_unknown(PAIR, 3, ARRAY))),
      "Pair with unknown optional body key 3 holding an array: skipped")
CMD = uint(5, 2) + kv([(0, u(42)), (1, u(2))])
CMD_REPLY = uint(5, 3) + kv([(0, u(42)), (1, u(2)), (2, u(1))])
write("command-v1-unknown-body-field.hex",
      frame(envelope(5, with_unknown(CMD, 3, ARRAY))),
      "Command with unknown optional body key 3 holding an array: skipped")
write("command-reply-v1-unknown-body-field.hex",
      frame(envelope(6, with_unknown(CMD_REPLY, 4, u(7)))),
      "CommandReply with unknown optional body key 4 holding an integer: skipped")
write("cpu-snapshot-v1-unknown-body-nested.hex",
      frame(envelope(4, with_unknown(CPU, 15, nest(3)))),
      "unknown snapshot-body field nested three arrays deep: at the depth limit, accepted")
write("cpu-snapshot-v1-unknown-body-too-deep.hex",
      frame(envelope(4, with_unknown(CPU, 15, nest(4)))),
      "unknown snapshot-body field nested four arrays deep: over the limit, malformed")
write("hello-v1-unknown-body-nested.hex",
      frame(envelope(1, with_unknown(HELLO, 9, nest(3)))),
      "unknown Hello-body field nested three arrays deep: accepted")
write("hello-v1-unknown-body-too-deep.hex",
      frame(envelope(1, with_unknown(HELLO, 9, nest(4)))),
      "unknown Hello-body field nested four arrays deep: malformed")

# Metric groups (C02): the snapshot body carries CPU at keys 0-4 and each further
# metric as an optional nested group at keys 5 and up. RAM is the first group.
def ram_group(items):
    return uint(5, len(items)) + kv(items)


def with_ram(group, extra_body=()):
    items = [(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4)), (5, group)]
    items += list(extra_body)
    return uint(5, len(items)) + kv(items)


GIB = 1 << 30
RAM = [(0, u(0)), (1, u(8 * GIB)), (2, u(16 * GIB)), (3, u(250)), (4, u(0))]
write("ram-snapshot-v1.hex", frame(envelope(4, with_ram(ram_group(RAM)))),
      "CPU plus RAM group: 8 GiB of 16 GiB, source age 250 ms")
write("ram-snapshot-v1-max-u64.hex",
      frame(envelope(4, with_ram(ram_group(
          [(0, u(0)), (1, u((1 << 64) - 1)), (2, u((1 << 64) - 1)), (3, u(0)), (4, u(0))])))),
      "RAM used and total at the 64-bit maximum: valid")
write("ram-snapshot-v1-used-exceeds-total.hex",
      frame(envelope(4, with_ram(ram_group(
          [(0, u(0)), (1, u(17 * GIB)), (2, u(16 * GIB)), (3, u(0)), (4, u(0))])))),
      "RAM used larger than total: malformed")
write("ram-snapshot-v1-missing-field.hex",
      frame(envelope(4, with_ram(ram_group(RAM[:4])))),
      "RAM group without freshness (key 4): malformed")
write("ram-snapshot-v1-duplicate-key.hex",
      frame(envelope(4, with_ram(ram_group([RAM[0], RAM[1], RAM[1]] + RAM[2:])))),
      "RAM group key 1 twice: malformed")
write("ram-snapshot-v1-not-a-map.hex",
      frame(envelope(4, with_ram(uint(2, 3) + b"\x01\x02\x03"))),
      "RAM group is a byte string, not a field map: malformed")
write("ram-snapshot-v1-unknown-field.hex",
      frame(envelope(4, with_ram(ram_group(RAM + [(6, uint(4, 2) + u(1) + u(2))])))),
      "unknown optional RAM field 6 holding [1, 2]: skipped, snapshot applies")
write("ram-snapshot-v1-unknown-too-deep.hex",
      frame(envelope(4, with_ram(ram_group(RAM + [(6, nest(4))])))),
      "unknown RAM field nested four arrays deep: over the skip limit, malformed")
write("ram-snapshot-v1-bad-availability.hex",
      frame(envelope(4, with_ram(ram_group([(0, u(5))] + RAM[1:])))),
      "RAM availability 5 is not a defined value: malformed")
write("ram-snapshot-v1-bad-freshness.hex",
      frame(envelope(4, with_ram(ram_group(RAM[:4] + [(4, u(2))])))),
      "RAM freshness 2 is not a defined value: malformed")
write("ram-snapshot-v1-age-overflow.hex",
      frame(envelope(4, with_ram(ram_group(RAM[:3] + [(3, u(1 << 32)), RAM[4]])))),
      "RAM source age above 32 bits: malformed")
write("ram-snapshot-v1-noncanonical.hex",
      frame(envelope(4, with_ram(ram_group(RAM[:1] + [(1, b"\x18\x05")] + RAM[2:])))),
      "RAM used encoded with a needless extra byte: non-canonical, malformed")
write("ram-snapshot-v1-duplicate-group.hex",
      frame(envelope(4, with_ram(ram_group(RAM), extra_body=[(5, ram_group(RAM))]))),
      "RAM group present twice in the snapshot body: malformed")
write("ram-snapshot-v1-unknown-nested.hex",
      frame(envelope(4, with_ram(ram_group(RAM + [(6, nest(3))])))),
      "unknown RAM field nested three arrays deep: depth counts from the value, so accepted"
      " (found by the parity gate: Rust once counted from the group map)")

# Local UTC offset (envelope key 8): minutes east of UTC, biased by +1440 so the
# wire value is an unsigned integer. 1440 is UTC, 1740 is UTC+5, 1140 is UTC-5.
write("cpu-snapshot-v1-offset-east.hex", frame(envelope(4, CPU, [(8, u(1740))])),
      "local UTC offset +300 minutes (wire 1740): accepted")
write("cpu-snapshot-v1-offset-west.hex", frame(envelope(4, CPU, [(8, u(1140))])),
      "local UTC offset -300 minutes (wire 1140): accepted")
write("cpu-snapshot-v1-offset-zero.hex", frame(envelope(4, CPU, [(8, u(1440))])),
      "local UTC offset 0 (wire 1440): accepted")
write("cpu-snapshot-v1-offset-out-of-range.hex", frame(envelope(4, CPU, [(8, u(2881))])),
      "local UTC offset wire value 2881 is beyond UTC+24h: malformed")
write("cpu-snapshot-v1-offset-not-uint.hex",
      frame(envelope(4, CPU, [(8, uint(4, 1) + u(1))])),
      "local UTC offset holding an array: malformed, not skipped as unknown")
write("cpu-snapshot-v1-offset-too-wide.hex", frame(envelope(4, CPU, [(8, u(1 << 32))])),
      "local UTC offset wider than 32 bits: malformed")
write("hello-v1-offset.hex", frame(envelope(1, HELLO, [(8, u(1740))])),
      "Hello carrying a local UTC offset in its envelope: accepted")


# Storage group (TB04), key 6: capacity and disk I/O with independent
# availability and source ages; no freshness field (the device derives it).
def with_storage(group, extra_body=(), ram=None):
    items = [(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4))]
    if ram is not None:
        items.append((5, ram))
    items.append((6, group))
    items += list(extra_body)
    return uint(5, len(items)) + kv(items)


U64 = (1 << 64) - 1
# 0 cap avail, 1 total, 2 available, 3 cap age, 4 io avail, 5 read/s, 6 write/s, 7 io age
STORAGE = [(0, u(0)), (1, u(500 * 10**9)), (2, u(200 * 10**9)), (3, u(1500)),
           (4, u(0)), (5, u(12_300_000)), (6, u(4_500_000)), (7, u(250))]


def storage_frame(items, **kw):
    return frame(envelope(4, with_storage(ram_group(items), **kw)))


def replaced(index, item):
    return STORAGE[:index] + [item] + STORAGE[index + 1:]


write("storage-snapshot-v1.hex", storage_frame(STORAGE),
      "CPU plus Storage group: 200 GB free of 500 GB (age 1.5 s), read 12.3 MB/s, write 4.5 MB/s (age 250 ms)")
write("storage-snapshot-v1-max-u64.hex",
      storage_frame([(0, u(0)), (1, u(U64)), (2, u(U64)), (3, u(0xffffffff)), (4, u(0)),
                     (5, u(U64)), (6, u(U64)), (7, u(0xffffffff))]),
      "Storage at every 64-bit and 32-bit maximum: valid, the largest group (58 bytes)")
write("storage-snapshot-v1-io-unavailable.hex",
      storage_frame([(0, u(0)), (1, u(500 * 10**9)), (2, u(200 * 10**9)), (3, u(1500)),
                     (4, u(3)), (5, u(0)), (6, u(0)), (7, u(0))]),
      "Storage capacity available while I/O is temporarily unavailable (zero values): valid")
write("storage-snapshot-v1-available-exceeds-total.hex",
      storage_frame(replaced(2, (2, u(501 * 10**9)))),
      "Storage available larger than total: malformed")
write("storage-snapshot-v1-missing-field.hex", storage_frame(STORAGE[:7]),
      "Storage group without I/O source age (key 7): malformed")
write("storage-snapshot-v1-duplicate-key.hex",
      storage_frame(STORAGE[:2] + [STORAGE[1]] + STORAGE[2:]),
      "Storage group key 1 twice: malformed")
write("storage-snapshot-v1-not-a-map.hex",
      frame(envelope(4, with_storage(uint(2, 3) + b"\x01\x02\x03"))),
      "Storage group is a byte string, not a field map: malformed")
write("storage-snapshot-v1-unknown-field.hex",
      storage_frame(STORAGE + [(8, uint(4, 2) + u(1) + u(2))]),
      "unknown optional Storage field 8 holding [1, 2]: skipped, snapshot applies")
write("storage-snapshot-v1-unknown-too-deep.hex",
      storage_frame(STORAGE + [(8, nest(4))]),
      "unknown Storage field nested four arrays deep: over the skip limit, malformed")
write("storage-snapshot-v1-unknown-nested.hex",
      storage_frame(STORAGE + [(8, nest(3))]),
      "unknown Storage field nested three arrays deep: depth counts from the value, accepted")
write("storage-snapshot-v1-bad-capacity-availability.hex",
      storage_frame(replaced(0, (0, u(5)))),
      "Storage capacity availability 5 is not a defined value: malformed")
write("storage-snapshot-v1-bad-io-availability.hex",
      storage_frame(replaced(4, (4, u(5)))),
      "Storage I/O availability 5 is not a defined value: malformed")
write("storage-snapshot-v1-capacity-age-overflow.hex",
      storage_frame(replaced(3, (3, u(1 << 32)))),
      "Storage capacity source age above 32 bits: malformed")
write("storage-snapshot-v1-io-age-overflow.hex",
      storage_frame(replaced(7, (7, u(1 << 32)))),
      "Storage I/O source age above 32 bits: malformed")
write("storage-snapshot-v1-noncanonical.hex",
      storage_frame(replaced(5, (5, b"\x18\x05"))),
      "Storage read rate encoded with a needless extra byte: non-canonical, malformed")
write("storage-snapshot-v1-duplicate-group.hex",
      frame(envelope(4, with_storage(ram_group(STORAGE), extra_body=[(6, ram_group(STORAGE))]))),
      "Storage group present twice in the snapshot body: malformed")
write("storage-snapshot-v1-with-ram.hex",
      frame(envelope(4, with_storage(ram_group(STORAGE), ram=ram_group(RAM)))),
      "CPU plus both the RAM (key 5) and Storage (key 6) groups")
write("storage-snapshot-v1-with-bad-ram.hex",
      frame(envelope(4, with_storage(ram_group(STORAGE), ram=ram_group(RAM[:4])))),
      "valid Storage group beside a RAM group missing a field: malformed as a whole")


# Battery group (TB06), key 7: charge/state and condition halves with
# independent availability and source ages; no freshness field.
def with_battery(group, extra_body=(), ram=None, storage=None):
    items = [(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4))]
    if ram is not None:
        items.append((5, ram))
    if storage is not None:
        items.append((6, storage))
    items.append((7, group))
    items += list(extra_body)
    return uint(5, len(items)) + kv(items)


# 0 charge avail, 1 charge per-mille, 2 state, 3 minutes remaining, 4 charge age,
# 5 condition avail, 6 max capacity per-mille, 7 condition, 8 cycles, 9 condition age
BATTERY = [(0, u(0)), (1, u(870)), (2, u(1)), (3, u(95)), (4, u(250)),
           (5, u(0)), (6, u(910)), (7, u(1)), (8, u(312)), (9, u(1_800_000))]
BATTERY_ABSENT = [(0, u(2)), (1, u(0)), (2, u(0)), (3, u(0xFFFF)), (4, u(0)),
                  (5, u(2)), (6, u(0)), (7, u(0)), (8, u(0xFFFF)), (9, u(0))]


def battery_frame(items, **kw):
    return frame(envelope(4, with_battery(ram_group(items), **kw)))


def battery_replaced(index, item):
    return BATTERY[:index] + [item] + BATTERY[index + 1:]


write("battery-snapshot-v1.hex", battery_frame(BATTERY),
      "CPU plus Battery group: 87% charging, 95 min to full (age 250 ms); health 91% normal, 312 cycles (age 30 min)")
write("battery-snapshot-v1-absent.hex", battery_frame(BATTERY_ABSENT),
      "Battery group on a batteryless host: charge and condition both Absent, zero/unknown values")
write("battery-snapshot-v1-condition-unavailable.hex",
      battery_frame(BATTERY[:5] + [(5, u(3)), (6, u(0)), (7, u(0)), (8, u(0xFFFF)), (9, u(0))]),
      "Battery charge available while the condition source is temporarily unavailable: valid")
write("battery-snapshot-v1-max.hex",
      battery_frame([(0, u(4)), (1, u(1000)), (2, u(4)), (3, u(0xFFFF)), (4, u(0xffffffff)),
                     (5, u(4)), (6, u(1000)), (7, u(3)), (8, u(0xFFFF)), (9, u(0xffffffff))]),
      "Battery at every maximum: valid, the largest group (38 bytes)")
write("battery-snapshot-v1-with-groups.hex",
      frame(envelope(4, with_battery(ram_group(BATTERY), ram=ram_group(RAM), storage=ram_group(STORAGE)))),
      "CPU plus the RAM (5), Storage (6) and Battery (7) groups")
write("battery-snapshot-v1-with-bad-storage.hex",
      frame(envelope(4, with_battery(ram_group(BATTERY), storage=ram_group(STORAGE[:7])))),
      "valid Battery group beside a Storage group missing a field: malformed as a whole")
write("battery-snapshot-v1-charge-over-1000.hex",
      battery_frame(battery_replaced(1, (1, u(1001)))),
      "Battery charge above 1000 per-mille: malformed")
write("battery-snapshot-v1-capacity-over-1000.hex",
      battery_frame(battery_replaced(6, (6, u(1001)))),
      "Battery maximum capacity above 1000 per-mille: malformed")
write("battery-snapshot-v1-bad-state.hex",
      battery_frame(battery_replaced(2, (2, u(5)))),
      "Battery state 5 is not a defined value: malformed")
write("battery-snapshot-v1-bad-condition.hex",
      battery_frame(battery_replaced(7, (7, u(4)))),
      "Battery condition 4 is not a defined value: malformed")
write("battery-snapshot-v1-time-over-16-bits.hex",
      battery_frame(battery_replaced(3, (3, u(0x10000)))),
      "Battery time remaining above 16 bits: malformed")
write("battery-snapshot-v1-cycles-over-16-bits.hex",
      battery_frame(battery_replaced(8, (8, u(0x10000)))),
      "Battery cycle count above 16 bits: malformed")
write("battery-snapshot-v1-bad-charge-availability.hex",
      battery_frame(battery_replaced(0, (0, u(5)))),
      "Battery charge availability 5 is not a defined value: malformed")
write("battery-snapshot-v1-bad-condition-availability.hex",
      battery_frame(battery_replaced(5, (5, u(5)))),
      "Battery condition availability 5 is not a defined value: malformed")
write("battery-snapshot-v1-charge-age-overflow.hex",
      battery_frame(battery_replaced(4, (4, u(1 << 32)))),
      "Battery charge source age above 32 bits: malformed")
write("battery-snapshot-v1-condition-age-overflow.hex",
      battery_frame(battery_replaced(9, (9, u(1 << 32)))),
      "Battery condition source age above 32 bits: malformed")
write("battery-snapshot-v1-missing-field.hex", battery_frame(BATTERY[:9]),
      "Battery group without condition source age (key 9): malformed")
write("battery-snapshot-v1-duplicate-key.hex",
      battery_frame(BATTERY[:2] + [BATTERY[1]] + BATTERY[2:]),
      "Battery group key 1 twice: malformed")
write("battery-snapshot-v1-not-a-map.hex",
      frame(envelope(4, with_battery(uint(2, 3) + b"\x01\x02\x03"))),
      "Battery group is a byte string, not a field map: malformed")
write("battery-snapshot-v1-unknown-field.hex",
      battery_frame(BATTERY + [(10, uint(4, 2) + u(1) + u(2))]),
      "unknown optional Battery field 10 holding [1, 2]: skipped, snapshot applies")
write("battery-snapshot-v1-unknown-too-deep.hex",
      battery_frame(BATTERY + [(10, nest(4))]),
      "unknown Battery field nested four arrays deep: over the skip limit, malformed")
write("battery-snapshot-v1-noncanonical.hex",
      battery_frame(battery_replaced(1, (1, b"\x18\x05"))),
      "Battery charge encoded with a needless extra byte: non-canonical, malformed")
write("battery-snapshot-v1-duplicate-group.hex",
      frame(envelope(4, with_battery(ram_group(BATTERY), extra_body=[(7, ram_group(BATTERY))]))),
      "Battery group present twice in the snapshot body: malformed")


# Agents group (TB07), key 8: availability, source age and three fixed agent
# state slots [codex, claude, grok]; no freshness field.
def with_agents(group, extra_body=(), ram=None, storage=None, battery=None):
    items = [(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4))]
    if ram is not None:
        items.append((5, ram))
    if storage is not None:
        items.append((6, storage))
    if battery is not None:
        items.append((7, battery))
    items.append((8, group))
    items += list(extra_body)
    return uint(5, len(items)) + kv(items)


# 0 availability, 1 source age ms, 2 codex, 3 claude, 4 grok
AGENTS = [(0, u(0)), (1, u(250)), (2, u(0)), (3, u(1)), (4, u(0))]


def agents_frame(items, **kw):
    return frame(envelope(4, with_agents(ram_group(items), **kw)))


def agents_replaced(index, item):
    return AGENTS[:index] + [item] + AGENTS[index + 1:]


write("agents-snapshot-v1.hex", agents_frame(AGENTS),
      "CPU plus Agents group: Claude running with activity unknown, Codex and Grok Build not running (age 250 ms)")
write("agents-snapshot-v1-none.hex",
      agents_frame([(0, u(0)), (1, u(0)), (2, u(0)), (3, u(0)), (4, u(0))]),
      "Agents group with the source available and no agent running")
write("agents-snapshot-v1-all-running.hex",
      agents_frame([(0, u(0)), (1, u(1200)), (2, u(1)), (3, u(1)), (4, u(1))]),
      "Agents group with all three agents running, activity unknown")
write("agents-snapshot-v1-busy-idle.hex",
      agents_frame([(0, u(0)), (1, u(300)), (2, u(2)), (3, u(3)), (4, u(1))]),
      "Agents group with busy (2) and idle (3) states, which only a future adapter supplies")
write("agents-snapshot-v1-unavailable.hex",
      agents_frame([(0, u(3)), (1, u(0)), (2, u(0)), (3, u(0)), (4, u(0))]),
      "Agents source temporarily unavailable: zero states, never 'none running'")
write("agents-snapshot-v1-max.hex",
      agents_frame([(0, u(4)), (1, u(0xffffffff)), (2, u(3)), (3, u(3)), (4, u(3))]),
      "Agents at every maximum: valid, the largest group (16 bytes)")
write("agents-snapshot-v1-with-groups.hex",
      frame(envelope(4, with_agents(ram_group(AGENTS), ram=ram_group(RAM), storage=ram_group(STORAGE),
                                    battery=ram_group(BATTERY)))),
      "CPU plus the RAM (5), Storage (6), Battery (7) and Agents (8) groups")
write("agents-snapshot-v1-with-bad-battery.hex",
      frame(envelope(4, with_agents(ram_group(AGENTS), battery=ram_group(BATTERY[:9])))),
      "valid Agents group beside a Battery group missing a field: malformed as a whole")
write("agents-snapshot-v1-bad-state.hex",
      agents_frame(agents_replaced(3, (3, u(4)))),
      "Agents state 4 is not a defined value: malformed")
write("agents-snapshot-v1-bad-availability.hex",
      agents_frame(agents_replaced(0, (0, u(5)))),
      "Agents availability 5 is not a defined value: malformed")
write("agents-snapshot-v1-age-overflow.hex",
      agents_frame(agents_replaced(1, (1, u(1 << 32)))),
      "Agents source age above 32 bits: malformed")
write("agents-snapshot-v1-missing-field.hex", agents_frame(AGENTS[:4]),
      "Agents group without the grok slot (key 4): malformed")
write("agents-snapshot-v1-duplicate-key.hex",
      agents_frame(AGENTS[:3] + [AGENTS[2]] + AGENTS[3:]),
      "Agents group key 2 twice: malformed")
write("agents-snapshot-v1-not-a-map.hex",
      frame(envelope(4, with_agents(uint(2, 3) + b"\x01\x02\x03"))),
      "Agents group is a byte string, not a field map: malformed")
write("agents-snapshot-v1-unknown-field.hex",
      agents_frame(AGENTS + [(5, uint(4, 2) + u(1) + u(2))]),
      "unknown optional Agents field 5 holding [1, 2]: skipped, snapshot applies")
write("agents-snapshot-v1-unknown-too-deep.hex",
      agents_frame(AGENTS + [(5, nest(4))]),
      "unknown Agents field nested four arrays deep: over the skip limit, malformed")
write("agents-snapshot-v1-noncanonical.hex",
      agents_frame(agents_replaced(1, (1, b"\x18\x05"))),
      "Agents source age encoded with a needless extra byte: non-canonical, malformed")
write("agents-snapshot-v1-duplicate-group.hex",
      frame(envelope(4, with_agents(ram_group(AGENTS), extra_body=[(8, ram_group(AGENTS))]))),
      "Agents group present twice in the snapshot body: malformed")


# Network group (TB05), key 9: thirteen fields, two independent halves.
# 0 traffic availability, 1 traffic age ms, 2 route kind (0 unknown, 1 WiFi,
# 3 other, 2 Ethernet, 4 none), 3 via tunnel, 4 rx B/s, 5 tx B/s,
# 6 association context availability (0-6), 7 context age ms, 8 coverage
# (0 unknown, 1 observable, 2 outside), 9 channel, 10 station (byte string of
# 0 or 6), 11 SSID (0-32 bytes), 12 BSSID (0 or 6), and the optional 13 active
# physical links (byte string of 0-8 kinds, each 1 WiFi, 2 Ethernet, 3 other).
def with_network(group, extra_body=(), ram=None, storage=None, battery=None, agents=None):
    items = [(0, u(0)), (1, u(723)), (2, u(1000)), (3, u(0)), (4, u(4))]
    for key, value in ((5, ram), (6, storage), (7, battery), (8, agents)):
        if value is not None:
            items.append((key, value))
    items.append((9, group))
    items += list(extra_body)
    return uint(5, len(items)) + kv(items)


def bstr(b):
    return uint(2, len(b)) + b


STATION = bytes.fromhex("f2a1b3c4d5e6")
BSSID = bytes.fromhex("0a1b2c3d4e5f")
NET = [(0, u(0)), (1, u(250)), (2, u(1)), (3, u(0)), (4, u(1_200_000)), (5, u(340_000)),
       (6, u(0)), (7, u(400)), (8, u(1)), (9, u(6)), (10, bstr(STATION)),
       (11, bstr(b"CYD-Test")), (12, bstr(BSSID))]


def network_frame(items, **kw):
    return frame(envelope(4, with_network(ram_group(items), **kw)))


def net_with(**fields):
    """NET with the named keys replaced; keys are given as k<number>."""
    out = []
    for key, value in NET:
        out.append((key, fields.get(f"k{key}", value)))
    return out


write("network-snapshot-v1.hex", network_frame(NET),
      "CPU plus Network group: WiFi primary route 1.2 MB/s down 340 kB/s up, channel 6 observable, "
      "context available (SSID CYD-Test), station and BSSID present")
write("network-snapshot-v1-offline.hex",
      network_frame(net_with(k0=u(2), k1=u(0), k2=u(4), k4=u(0), k5=u(0), k6=u(1), k7=u(0),
                             k8=u(0), k9=u(0), k10=bstr(b""), k11=bstr(b""), k12=bstr(b""))),
      "Network with no primary route: kind 4, traffic Absent with zero rates, no station")
write("network-snapshot-v1-ethernet.hex",
      network_frame(net_with(k2=u(2), k6=u(5), k8=u(0), k9=u(0), k10=bstr(b""),
                             k11=bstr(b""), k12=bstr(b""))),
      "Network over Ethernet: rates, WiFi context unsupported and coverage unknown")
write("network-snapshot-v1-tunnel.hex",
      network_frame(net_with(k3=u(1))),
      "Network with a tunnel as primary route (underlying WiFi): the tunnel's own rate, via_tunnel 1")
write("network-snapshot-v1-rate-unavailable.hex",
      network_frame(net_with(k0=u(3), k1=u(0), k4=u(0), k5=u(0))),
      "Network right after a route change: kind WiFi, rate temporarily unavailable with zero values")
write("network-snapshot-v1-5ghz-outside.hex",
      network_frame(net_with(k6=u(2), k8=u(2), k9=u(149), k11=bstr(b""), k12=bstr(b""))),
      "Network on channel 149: outside supported coverage, permission denied (no SSID/BSSID), station present")
write("network-snapshot-v1-context-unknown.hex",
      network_frame(net_with(k6=u(1), k7=u(0), k8=u(0), k9=u(0), k11=bstr(b""), k12=bstr(b""))),
      "Network with no provider: context not requested, coverage unknown, station identity still present")
write("network-snapshot-v1-band-only.hex",
      network_frame(net_with(k6=u(4), k9=u(0), k11=bstr(b""), k12=bstr(b""))),
      "Network with a known 2.4 GHz band but no channel and authorized-but-unavailable SSID")
write("network-snapshot-v1-rate-error.hex",
      network_frame(net_with(k0=u(4), k2=u(0), k4=u(0), k5=u(0))),
      "Network traffic read error: unknown route kind, zero rates")
for n, name in enumerate(["available", "not-requested", "denied", "restricted",
                          "authorized-unavailable", "unsupported", "error"]):
    extra = {} if n == 0 else dict(k11=bstr(b""), k12=bstr(b""))
    write(f"network-snapshot-v1-ctx-{name}.hex",
          network_frame(net_with(k6=u(n), **extra)),
          f"Network Association Context Availability {n} ({name.replace('-', ' ')})")
write("network-snapshot-v1-max.hex",
      network_frame([(0, u(0)), (1, u(0xffffffff)), (2, u(3)), (3, u(1)), (4, u(2**64 - 1)),
                     (5, u(2**64 - 1)), (6, u(0)), (7, u(0xffffffff)), (8, u(2)), (9, u(233)),
                     (10, bstr(STATION)), (11, bstr(bytes(range(0x80, 0xa0)))), (12, bstr(BSSID)),
                     (13, bstr(bytes([1, 2, 3, 3, 3, 3, 3, 3])))]),
      "Network at every maximum: 64-bit rates, 32-bit ages, 32-byte SSID, eight links: valid, the "
      "largest group (108 bytes)")
write("network-snapshot-v1-links.hex",
      network_frame(NET + [(13, bstr(b"\x01\x02"))]),
      "Network with WiFi and Ethernet both active: the rates are their totals, one link each")
write("network-snapshot-v1-links-empty.hex",
      network_frame(NET + [(13, bstr(b""))]),
      "Network reporting no active physical link (empty key 13): valid")
write("network-snapshot-v1-with-groups.hex",
      frame(envelope(4, with_network(ram_group(NET), ram=ram_group(RAM), storage=ram_group(STORAGE),
                                     battery=ram_group(BATTERY), agents=ram_group(AGENTS)))),
      "CPU plus the RAM (5), Storage (6), Battery (7), Agents (8) and Network (9) groups")
write("network-snapshot-v1-with-bad-agents.hex",
      frame(envelope(4, with_network(ram_group(NET), agents=ram_group(AGENTS[:4])))),
      "valid Network group beside an Agents group missing a field: malformed as a whole")
bad = {
    "bad-traffic-availability": (net_with(k0=u(5)), "traffic availability 5 is not defined"),
    "bad-route-kind": (net_with(k2=u(5)), "route kind 5 is not defined"),
    "bad-via-tunnel": (net_with(k3=u(2)), "via_tunnel 2 is not a boolean"),
    "bad-context": (net_with(k6=u(7)), "association context availability 7 is not defined"),
    "bad-coverage": (net_with(k8=u(3)), "coverage 3 is not defined"),
    "channel-over-255": (net_with(k9=u(256)), "channel above 8 bits"),
    "channel-without-coverage": (net_with(k8=u(0)), "a channel with coverage unknown"),
    "ssid-oversize": (net_with(k11=bstr(bytes(33))), "SSID of 33 bytes, over the 32-byte bound"),
    "ssid-without-context": (net_with(k6=u(2)), "SSID present while permission is denied"),
    "bssid-without-context": (net_with(k6=u(4), k11=bstr(b"")), "BSSID present while context is unavailable"),
    "bad-station-length": (net_with(k10=bstr(STATION[:5])), "station identity of 5 bytes"),
    "bad-bssid-length": (net_with(k12=bstr(BSSID + b"\x00")), "BSSID of 7 bytes"),
    "rates-with-unavailable": (net_with(k0=u(3)), "non-zero rates while traffic is unavailable"),
    "tunnel-without-route": (net_with(k2=u(4), k0=u(2), k4=u(0), k5=u(0), k3=u(1)),
                             "via_tunnel with no primary route"),
    "traffic-age-overflow": (net_with(k1=u(1 << 32)), "traffic age above 32 bits"),
    "context-age-overflow": (net_with(k7=u(1 << 32)), "context age above 32 bits"),
    "station-as-uint": (net_with(k10=u(5)), "station encoded as an integer, not a byte string"),
    "ssid-as-text": (net_with(k11=uint(3, 3) + b"abc"), "SSID encoded as a text string"),
    "missing-field": (NET[:12], "Network group without the BSSID (key 12)"),
    "duplicate-key": (NET[:3] + [NET[2]] + NET[3:], "Network key 2 twice"),
    "noncanonical": (net_with(k1=b"\x18\x05"), "traffic age encoded with a needless extra byte"),
    "bad-link-kind": (NET + [(13, bstr(b"\x01\x04"))], "link kind 4 (none) is not an active link"),
    "link-kind-zero": (NET + [(13, bstr(b"\x00"))], "link kind 0 (unknown) is not an active link"),
    "links-oversize": (NET + [(13, bstr(bytes([2] * 9)))], "nine links, over the bound of eight"),
    "links-as-uint": (NET + [(13, u(1))], "links encoded as an integer, not a byte string"),
    "links-duplicate": (NET + [(13, bstr(b"\x01")), (13, bstr(b"\x02"))], "Network key 13 twice"),
}
for name, (items, note) in bad.items():
    write(f"network-snapshot-v1-{name}.hex", network_frame(items), f"{note}: malformed")
write("network-snapshot-v1-not-a-map.hex",
      frame(envelope(4, with_network(uint(2, 3) + b"\x01\x02\x03"))),
      "Network group is a byte string, not a field map: malformed")
write("network-snapshot-v1-unknown-field.hex",
      network_frame(NET + [(14, uint(4, 2) + u(1) + u(2))]),
      "unknown optional Network field 14 holding [1, 2]: skipped, snapshot applies")
write("network-snapshot-v1-unknown-too-deep.hex",
      network_frame(NET + [(14, nest(4))]),
      "unknown Network field nested four arrays deep: over the skip limit, malformed")
write("network-snapshot-v1-duplicate-group.hex",
      frame(envelope(4, with_network(ram_group(NET), extra_body=[(9, ram_group(NET))]))),
      "Network group present twice in the snapshot body: malformed")
