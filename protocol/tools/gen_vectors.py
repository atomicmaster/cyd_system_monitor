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
      frame(envelope(4, CPU, [(8, uint(4, 2) + uint(4, 2) + u(1) + u(2) + uint(4, 2) + u(3) + u(4))])),
      "unknown optional key 8 holding [[1,2],[3,4]]: skipped, snapshot applies")
write("cpu-snapshot-v1-large-unknown.hex",
      frame(envelope(4, CPU, [(8, uint(2, 1100) + bytes(1100))])),
      "unknown optional key 8 holding 1,100 bytes: a ~1.2 KB frame, over the old 1,024 cap")
write("cpu-snapshot-v1-too-deep.hex",
      frame(envelope(4, CPU, [(8, nest(4))])),
      "unknown optional key 8 nested four arrays deep: over the skip limit")
write("cpu-snapshot-v1-tag.hex",
      frame(envelope(4, CPU, [(8, b"\xc0\x00")])),
      "unknown optional key 8 holding a CBOR tag: outside the profile")
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
                               (9, uint(4, 2) + u(1) + uint(3, 1) + b"x")])
write("cpu-snapshot-v1-unknown-body-field.hex", frame(envelope(4, CPU_UNKNOWN)),
      "unknown optional snapshot-body field 9 holding [1, \"x\"]: skipped like any bounded value")
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
      frame(envelope(4, with_unknown(CPU, 9, nest(3)))),
      "unknown snapshot-body field nested three arrays deep: at the depth limit, accepted")
write("cpu-snapshot-v1-unknown-body-too-deep.hex",
      frame(envelope(4, with_unknown(CPU, 9, nest(4)))),
      "unknown snapshot-body field nested four arrays deep: over the limit, malformed")
write("hello-v1-unknown-body-nested.hex",
      frame(envelope(1, with_unknown(HELLO, 9, nest(3)))),
      "unknown Hello-body field nested three arrays deep: accepted")
write("hello-v1-unknown-body-too-deep.hex",
      frame(envelope(1, with_unknown(HELLO, 9, nest(4)))),
      "unknown Hello-body field nested four arrays deep: malformed")
