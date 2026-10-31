# SPDX-License-Identifier: GPL-3.0-only
"""Reset the board, log serial output, and send DEV: commands at given times.
Usage: serial_session.py PORT TOTAL_SECONDS [T:COMMAND ...] [--no-reset] [--log FILE]"""
import sys, time
try:
    import serial
except ImportError:
    sys.exit("pyserial is missing; use ESP-IDF\'s Python ($IDF_PYTHON_ENV_PATH/bin/python after sourcing export.sh).")

args = sys.argv[1:]
no_reset = "--no-reset" in args
if no_reset:
    args.remove("--no-reset")
log = None
if "--log" in args:
    i = args.index("--log")
    log = open(args[i + 1], "w", encoding="utf-8")
    del args[i:i + 2]

def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()
    if log:
        log.write(text)
        log.flush()

port, total = args[0], float(args[1])
plan = []
for arg in args[2:]:
    t, cmd = arg.split(":", 1)
    plan.append((float(t), cmd))
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.1
s.dtr = False
s.rts = False
s.open()
if not no_reset:
    s.rts = True; time.sleep(0.1); s.rts = False  # pulse EN
start = time.time()
buf = b""
while time.time() - start < total:
    now = time.time() - start
    while plan and plan[0][0] <= now:
        _, cmd = plan.pop(0)
        emit(f"\n>>> [{now:6.2f}s] {cmd}\n")
        s.write((cmd + "\n").encode())
    data = s.read(4096)
    if data:
        buf += data
        *lines, buf = buf.split(b"\n")
        for line in lines:
            emit(f"[{time.time()-start:6.2f}] " + line.decode("utf-8", "replace").rstrip("\r") + "\n")
        sys.stdout.flush()
s.close()

if log:
    log.close()
