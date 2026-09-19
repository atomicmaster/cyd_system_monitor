#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Differential parity gate between the C++ and Rust protocol codecs.

Builds a deterministic corpus (every fixture plus systematic and seeded-random
mutations, as single frames and as byte streams), runs both codecs' parity
CLIs over it, and fails on any line where the outcome or re-encoded bytes
differ. A mismatch is a protocol misalignment: per protocol/CORE_PROTOCOL.md's
change gate, first add a failing test/fixture row for the input, then fix.

  parity_diff.py [--seed N] [--random N] [--show N]
"""
import argparse
import pathlib
import random
import subprocess
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "protocol" / "fixtures"
CPP = ROOT / "protocol" / "tests" / "cpp" / "build" / "protocol_parity_cli"
RUST = ["cargo", "run", "-q", "--manifest-path", str(ROOT / "host" / "Cargo.toml"),
        "-p", "protocol", "--bin", "parity_cli"]
# Interesting CBOR heads: ints of each width, strings, arrays/maps, indefinite
# lengths, tags, simple values, break.
POKES = [0x00, 0x01, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x20, 0x40, 0x41, 0x50, 0x58, 0x60,
         0x80, 0x81, 0x90, 0x9F, 0xA0, 0xA1, 0xA8, 0xA9, 0xB0, 0xBF, 0xC0, 0xC1, 0xE0, 0xF4,
         0xF6, 0xF9, 0xFA, 0xFB, 0xFF]


def fixture_frames():
    frames = []
    for path in sorted(FIXTURES.glob("*.hex")):
        text = "".join(l for l in path.read_text().splitlines() if not l.startswith("#"))
        frames.append(bytes.fromhex(text.strip()))
    return frames


def refit(cbor):
    """A frame with a correct length and CRC around arbitrary CBOR bytes."""
    return (len(cbor) + 4).to_bytes(2, "big") + cbor + zlib.crc32(cbor).to_bytes(4, "big")


def body_of(frame):
    return frame[2:-4]


def systematic(frame):
    """Mutations that keep the frame well-formed at the framing layer, so the
    CBOR/body decoders (where divergences live) are the ones exercised."""
    cbor = body_of(frame)
    for i in range(len(cbor)):
        for value in set(POKES) | {cbor[i] ^ 1, (cbor[i] + 1) & 0xFF, cbor[i] ^ 0x80}:
            yield refit(cbor[:i] + bytes([value]) + cbor[i + 1:])
        yield refit(cbor[:i] + cbor[i + 1:])
        for value in (0x00, 0x01, 0x18, 0xFF):
            yield refit(cbor[:i] + bytes([value]) + cbor[i:])
    # Framing-layer damage.
    for i in range(len(frame)):
        yield frame[:i]
        yield frame[:i] + bytes([frame[i] ^ 0x01]) + frame[i + 1:]
    yield frame + b"\x00"
    for length in (0, 1, 4, 5, 6, 2045, 2046, 2047, 2048, 65535):
        yield length.to_bytes(2, "big") + frame[2:]


def item_end(buf, pos):
    """End offset of the canonical-CBOR item at pos (fixtures are canonical)."""
    head, arg = buf[pos] >> 5, buf[pos] & 31
    pos += 1
    width = {24: 1, 25: 2, 26: 4, 27: 8}.get(arg, 0)
    value = arg if arg < 24 else int.from_bytes(buf[pos:pos + width], "big")
    pos += width
    if head in (2, 3):
        return pos + value
    for _ in range(value * (2 if head == 5 else 1) if head in (4, 5) else 0):
        pos = item_end(buf, pos)
    return pos


def uint_head(major, v):
    if v < 24:
        return bytes([major << 5 | v])
    for arg, n in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if v < 1 << (8 * n):
            return bytes([major << 5 | arg]) + v.to_bytes(n, "big")


def nest(depth, inner=b"\x00"):
    return b"\x81" * depth + inner


UNKNOWN_VALUES = [
    uint_head(0, v) for v in (0, 23, 24, 255, 256, 65535, 65536, 2**32 - 1, 2**32, 2**64 - 1)
] + [uint_head(1, 0), uint_head(1, 300), b"\x40", b"\x41\x00", uint_head(2, 600) + bytes(600),
     uint_head(2, 2500) + bytes(2500), b"\x60", b"\x61x", uint_head(3, 700) + b"y" * 700,
     b"\x80", nest(1), nest(3), nest(4), nest(5), uint_head(4, 16) + b"\x00" * 16,
     uint_head(4, 17) + b"\x00" * 17, b"\xa0", b"\xa1\x00\x00", b"\xa1\x00" + nest(3),
     b"\xc0\x00", b"\xc1\x1a\x00\x00\x00\x01", b"\xf4", b"\xf5", b"\xf6", b"\xf7",
     b"\xf9\x3c\x00", b"\xfa\x3f\x80\x00\x00", b"\xfb" + bytes(8), b"\x9f\x00\xff",
     b"\x5f\x41\x00\xff", b"\xbf\x00\x00\xff", b"\xff", b"\x1c", b"\x1f", b"\x18\x05",
     b"\x19\x00\x05", b"\x1a\x00\x00\x00\x05", b"\x1b" + bytes(7) + b"\x05"]


def extensions(frame):
    """Add one unknown (or duplicate-ish) entry to the envelope or the body map."""
    cbor = body_of(frame)
    if cbor[0] >> 5 != 5 or cbor[0] & 31 >= 23:
        return
    count, pos, key_end, body_span = cbor[0] & 31, 1, [], None
    for _ in range(count):
        key = cbor[pos]
        pos = item_end(cbor, pos)  # key
        start = pos
        pos = item_end(cbor, pos)  # value
        if key == 5:
            body_span = (start, pos)
    # Envelope-level: unknown keys after the last known key (canonical order).
    for key in (6, 7, 8, 9, 20, 300):
        for value in UNKNOWN_VALUES:
            entry = uint_head(0, key) + value
            yield refit(bytes([cbor[0] + 1]) + cbor[1:] + entry)
    if body_span and cbor[body_span[0]] >> 5 == 5 and cbor[body_span[0]] & 31 < 23:
        start, end = body_span
        for key in (4, 5, 6, 9, 30, 1000):
            for value in UNKNOWN_VALUES:
                entry = uint_head(0, key) + value
                body = bytes([cbor[start] + 1]) + cbor[start + 1:end] + entry
                yield refit(cbor[:start] + body + cbor[end:])


def random_mutants(frames, rng, count):
    for _ in range(count):
        cbor = bytearray(body_of(rng.choice(frames)))
        for _ in range(rng.randint(1, 4)):
            if not cbor:
                break
            op, i = rng.randrange(4), rng.randrange(len(cbor))
            if op == 0:
                cbor[i] = rng.choice(POKES + [rng.randrange(256)])
            elif op == 1:
                del cbor[i]
            elif op == 2:
                cbor.insert(i, rng.choice(POKES + [rng.randrange(256)]))
            else:
                j = rng.randrange(len(cbor))
                cbor[i], cbor[j] = cbor[j], cbor[i]
        yield refit(bytes(cbor))


def streams(frames, rng, count):
    junk = [b"", b"\xff", b"\xff\xff", b"\x00\x00\x08", b"\x00\x2f", b"\x01\x00", b"\x00\x05\x00"]
    for _ in range(count):
        parts = []
        for _ in range(rng.randint(1, 4)):
            parts.append(rng.choice(junk))
            frame = bytearray(rng.choice(frames))
            if rng.random() < 0.3 and frame:
                frame[rng.randrange(len(frame))] ^= 1 << rng.randrange(8)
            parts.append(bytes(frame))
        parts.append(rng.choice(junk))
        yield b"".join(parts)


def corpus(seed, random_count):
    frames = fixture_frames()
    rng = random.Random(seed)
    lines = [f"F {f.hex()}" for f in frames]
    for frame in frames:
        lines += [f"F {m.hex()}" for m in systematic(frame)]
    for frame in frames:
        try:
            lines += [f"F {m.hex()}" for m in list(extensions(frame))]
        except IndexError:  # a deliberately malformed fixture: no structure to extend
            pass
    lines += [f"F {m.hex()}" for m in random_mutants(frames, rng, random_count)]
    for wire in streams(frames, rng, max(200, random_count // 4)):
        lines += [f"S {wire.hex()}", f"B {wire.hex()}"]
    return list(dict.fromkeys(lines))


def run(cmd, lines):
    proc = subprocess.run(cmd, input="\n".join(lines) + "\n", capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"parity: {cmd[0]} failed:\n{proc.stderr}")
    out = proc.stdout.splitlines()
    if len(out) != len(lines):
        sys.exit(f"parity: {cmd[0]} produced {len(out)} lines for {len(lines)} inputs")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--random", type=int, default=4000)
    ap.add_argument("--show", type=int, default=8)
    args = ap.parse_args()
    cpp_dir = CPP.parent.parent
    for cmd in (["cmake", "-S", str(cpp_dir), "-B", str(cpp_dir / "build"), "-G", "Ninja"],
                ["cmake", "--build", str(cpp_dir / "build"), "--target", "protocol_parity_cli"]):
        if subprocess.run(cmd, capture_output=True).returncode != 0:
            sys.exit("parity: could not build the C++ harness")
    lines = corpus(args.seed, args.random)
    cpp, rust = run([str(CPP)], lines), run(RUST, lines)
    bad = [(l, c, r) for l, c, r in zip(lines, cpp, rust) if c != r]
    for line, c, r in bad[: args.show]:
        print(f"MISMATCH {line[:200]}\n  c++ : {c[:300]}\n  rust: {r[:300]}")
    accepted = sum(1 for c in cpp if c.startswith("ok"))
    print(f"parity: {len(lines)} inputs (seed {args.seed}), {accepted} accepted, "
          f"{len(bad)} mismatch(es)")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
