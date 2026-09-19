#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the board's CPU replay and diff its serial log against the simulator.

Sends DEV:CPU_REPLAY, captures the `cpu-replay: <checkpoint line>` log lines
until `cpu-replay: done`, and compares them with
simulator/scenarios/cpu-replay.expected (the desktop transcript of the same
script). Needs pyserial (ships in the ESP-IDF Python environment). The board
must already be flashed and showing the diagnostic screen (calibrated).

  cpu_replay_capture.py --port /dev/cu.usbserial-XXXX [--out capture.log]
"""
import argparse
import difflib
import pathlib
import re
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXPECTED = ROOT / "simulator" / "scenarios" / "cpu-replay.expected"
LINE = re.compile(r"cpu-replay: (.*)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the replay")
    ap.add_argument("--out", help="write the raw capture here")
    args = ap.parse_args()

    raw = []
    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        # Opening the port may reset the board through DTR/RTS; let it boot.
        time.sleep(4)
        port.reset_input_buffer()
        port.write(b"DEV:CPU_REPLAY\n")
        deadline = time.time() + args.timeout
        buffer = b""
        while time.time() < deadline:
            buffer += port.read(4096)
            *lines, buffer = buffer.split(b"\n")
            for line in lines:
                text = ANSI.sub("", line.decode("utf-8", "replace")).strip()
                raw.append(text)
                if "cpu-replay: ignored" in text:
                    print("cpu-replay was ignored: the board must show the calibrated diagnostic "
                          "screen (recalibrate first, or DEV:CLEAR_CALIBRATION then calibrate).")
                    return 2
                if "cpu-replay: done" in text:
                    deadline = 0
    if args.out:
        pathlib.Path(args.out).write_text("\n".join(raw) + "\n")

    got = [m.group(1) for line in raw if (m := LINE.search(line))
           and m.group(1) not in ("begin", "done") and not m.group(1).startswith("ignored")]
    want = EXPECTED.read_text().splitlines()
    if got == want:
        print(f"cpu-replay: board log matches the simulator transcript ({len(got)} checkpoints)")
        return 0
    print("cpu-replay: board log differs from simulator/scenarios/cpu-replay.expected")
    for line in difflib.unified_diff(want, got, "simulator", "board", lineterm=""):
        print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
