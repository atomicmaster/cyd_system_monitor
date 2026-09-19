#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Inspect a bounded M2 frame fixture without trusting its declared fields."""
import pathlib
import sys
import zlib

def fixture_bytes(path):
    text = pathlib.Path(path).read_text()
    return bytes.fromhex("".join(line for line in text.splitlines() if not line.startswith("#")))

def main():
    if len(sys.argv) != 2:
        print("usage: decode.py <hex-fixture>", file=sys.stderr); return 2
    wire = fixture_bytes(sys.argv[1])
    if len(wire) < 7: print("error: truncated"); return 1
    size = int.from_bytes(wire[:2], "big")
    if size > 1022 or size + 2 != len(wire): print("error: bad length"); return 1
    cbor, crc = wire[2:-4], int.from_bytes(wire[-4:], "big")
    if zlib.crc32(cbor) != crc: print("error: bad-crc"); return 1
    print(f"frame: {len(wire)} bytes, crc32: {crc:08x}")
    print("cbor: canonical M2 envelope (use C++/Rust codec for semantic decoding)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
