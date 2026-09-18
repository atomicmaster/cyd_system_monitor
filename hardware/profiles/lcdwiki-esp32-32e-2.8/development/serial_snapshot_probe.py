#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# F08 serial-headroom probe. Streams representative-sized, newline-framed
# lines ("SEQ:<n>:<filler>\n") to the E32R28T over its USB-serial link at a
# configurable cadence and size, standing in for C01's not-yet-built CBOR
# snapshot -- see snapshot-size-estimate.md for the byte-budget this tool's
# default size (1019) and frame count (60, matching firmware's fixed
# kSerialRxTestFrameCount) come from. It does not speak CBOR or the real
# protocol; it only exercises the UART link at a representative byte rate
# so DEV:SERIAL_RX_TEST on the firmware side can report bytes received,
# sequence gaps, and inter-frame timing.
#
# Usage (from the ESP-IDF python env, which already has pyserial):
#   $IDF_PYTHON_ENV_PATH/bin/python serial_snapshot_probe.py \
#       --port /dev/cu.usbserial-140 --size 1019 --rate-hz 1.0
#
# The board must already be running an image built with dev_console's
# DEV:SERIAL_RX_TEST command (this repo's default firmware build). This
# script sends the trigger command itself; it does not flash or monitor.

import argparse
import sys
import time

import serial

FRAME_COUNT = 60  # must match firmware's kSerialRxTestFrameCount


def build_frame(seq: int, target_size: int) -> bytes:
    """Builds one "SEQ:<n>:<filler>" line, sized to target_size bytes
    (excluding the trailing newline). Raises if the sequence number alone
    would not leave room for a non-negative filler."""
    prefix = f"SEQ:{seq}:".encode("ascii")
    filler_len = target_size - len(prefix)
    if filler_len < 0:
        raise ValueError(
            f"target size {target_size} too small for prefix {prefix!r} "
            f"({len(prefix)} bytes) at sequence {seq}"
        )
    return prefix + b"x" * filler_len + b"\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial device, e.g. /dev/cu.usbserial-140")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--size",
        type=int,
        default=1019,
        help="target frame size in bytes, excluding the newline "
        "(default: the text-encoded, 8-interface snapshot estimate)",
    )
    parser.add_argument(
        "--rate-hz",
        type=float,
        default=1.0,
        help="frames per second (default 1.0, matching the one-second snapshot cadence)",
    )
    parser.add_argument(
        "--skip-trigger",
        action="store_true",
        help="do not send DEV:SERIAL_RX_TEST first (board already waiting)",
    )
    args = parser.parse_args()

    period_s = 1.0 / args.rate_hz
    frames = [build_frame(seq, args.size) for seq in range(FRAME_COUNT)]
    total_bytes = sum(len(frame) for frame in frames)

    print(
        f"sending {FRAME_COUNT} frames of {args.size} bytes each "
        f"({total_bytes} bytes total) at {args.rate_hz:.2f} Hz "
        f"(~{FRAME_COUNT * period_s:.1f} s)"
    )

    ser = serial.Serial(args.port, args.baud, timeout=1)
    try:
        if not args.skip_trigger:
            ser.write(b"DEV:SERIAL_RX_TEST\n")
            ser.flush()
            time.sleep(0.2)  # let the board log its "waiting for N frames" line

        start = time.monotonic()
        for frame in frames:
            ser.write(frame)
            ser.flush()
            time.sleep(period_s)
        elapsed = time.monotonic() - start

        print(
            f"host side: sent {total_bytes} bytes in {elapsed:.2f} s "
            f"({total_bytes / elapsed:.0f} B/s)"
        )
        print("reading board response for 5 s (look for DEV:SERIAL_RX_STATUS)...")

        deadline = time.monotonic() + 5.0
        buf = b""
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                if b"DEV:SERIAL_RX_STATUS" in buf:
                    break
        sys.stdout.write(buf.decode("utf-8", "replace"))
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
