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
write("cpu-snapshot-v1-oversized-length.hex", frame(envelope(4, CPU), length=1024),
      "declared length 1024 exceeds the 1022-byte limit")
write("hello-reply-v1.hex", frame(envelope(2, HELLO)), "HelloReply, minor 0")
write("hello-v1-incompatible-major.hex", frame(envelope(1, HELLO, major=2)),
      "Hello from major 2: reply with Error, apply nothing")
write("hello-v1-wrong-body.hex", frame(envelope(1, uint(4, 1) + u(1))),
      "Hello whose body is an array, not a map")
write("error-v1.hex", frame(envelope(7, uint(2, 3) + b"\x01\x02\x03")),
      "Error carrying a 3-byte opaque diagnostic")
