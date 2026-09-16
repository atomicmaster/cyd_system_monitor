<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Development environment

Project-owned setup, build, flash, serial-debugging, and hardware smoke-test instructions for this profile belong here. ESP-IDF C++ and LVGL are selected; exact toolchain versions and reproducible setup remain M0 outputs.

Relevant unmodified vendor guides are under [`../vendor/development/`](../vendor/development/). The vendor package also contains Windows-only flashing tools; these are reference material and are not the planned macOS development workflow.

The M1a combined feasibility gate must resolve the three separately wired display/touch/MicroSD SPI buses against the ESP32's two general-purpose SPI controllers; software-driven touch SPI is a candidate, not a settled implementation. Record the chosen arrangement and measured touch/display behavior alongside radio capture, USB traffic, RAM/flash headroom, journal space, and write budgets. See [IMPLEMENTATION_PLAN.md](../../../../IMPLEMENTATION_PLAN.md).

The hardware smoke-test workflow must verify the XPT2046 interrupt and SPI pins, display orientation, guided first-start calibration, calibration persistence, and the deliberate recalibration recovery path.

## Build, flash, and monitor

Prerequisite: ESP-IDF v5.3.2 installed and `source $IDF_PATH/export.sh` run in the shell (see [docs/development.md](../../../../docs/development.md)). Confirm these with `./dev doctor`.

```sh
# Build the flashable image (no board required).
./dev build hardware      # equivalent to ./dev build firmware: idf.py build && idf.py size

# Native domain/calibration checks (no board, no ESP-IDF required).
./dev check hardware      # equivalent to ./dev check firmware

# Flash and monitor on a connected, exclusively claimed board.
# Confirm the actual serial port first -- it changes between plugs/reboots
# (macOS: ls /dev/cu.usbserial-*; the port observed at profile-authoring
# time was /dev/cu.usbserial-140, but do not assume it is still that).
./dev run hardware -- display-smoke --port /dev/cu.usbserial-XXX
./dev run hardware -- calibration   --port /dev/cu.usbserial-XXX
./dev run hardware -- peripherals   --port /dev/cu.usbserial-XXX
```

`./dev run hardware` flashes the single always-on diagnostic image (`idf.py -p "$port" flash monitor`) and opens the serial monitor; there is no firmware-side case switch. The `<case>` argument records which ticket's acceptance bullets the operator is exercising and evidencing during that session (F05/display-smoke, F06/calibration, F07/peripherals) -- see `tools/dev/hardware.sh` for the exact plumbing.

## BOOT/RESET recovery

If the board does not enter the ROM bootloader automatically for flashing (`idf.py flash` times out waiting to sync):

1. Hold the **BOOT** (download, GPIO0) button.
2. While still holding BOOT, briefly tap **RESET/EN** (the EN pin, shared with `buttons.reset` and `display.reset` in `profile.toml`).
3. Release **RESET/EN**, keep holding **BOOT** for another second, then release BOOT.
4. Retry `idf.py flash` (or `./dev run hardware -- ... --port ...`). The chip's ROM bootloader should now accept the serial handshake.

This is the standard ESP32 ROM-bootloader manual-recovery sequence (BOOT low during the EN/RESET rising edge selects the UART download boot mode), not a project-specific behavior.

## Smoke-test checklist

Record for each session, per `docs/tickets/README.md#dispatch-and-completion`: claimant, ticket, port, flashed commit, and release of the device.

- [ ] Display renders readable, correctly oriented 320x240 landscape diagnostics after a cold boot.
- [ ] Display renders correctly after a warm reset (RESET/EN tap) and after a repeated power cycle.
- [ ] UART (`idf.py monitor` / `ESP_LOGI`) reports the actual build identity, profile id, and reset reason, and the reset reason changes correctly between a cold boot and a RESET/EN tap.
- [ ] Backlight is on and steady; note any observed flicker or dimming behavior against the vendor claim.
- [ ] Touch: guided 5-point calibration completes on an uncalibrated panel; the validation pass accepts accurate taps and rejects deliberately inaccurate ones; a calibrated panel's calibration survives a power cycle; the `DEV:CLEAR_CALIBRATION` UART command (see `firmware/platform/esp32/dev_console/`) clears it and returns to the guided flow.
- [ ] RGB LED: each of red/green/blue is independently visible at the expected polarity (active-low, common anode).
- [ ] Audio: amplifier enable pin and DAC tone are verified deliberately (optional sound stays off by default); note the SC8002B vs FM8002E electrical-limit discrepancy from `audio-amplifier-comparison.md` if relevant to the fitted board.
- [ ] MicroSD: both an inserted, formatted card (mounts) and no card (ordinary idle status, not a failure) are exercised.
- [ ] Battery/supply voltage ADC reads a plausible millivolt value with the board on USB power; this is diagnostic-only, not a calibrated state of charge.
- [ ] BOOT button reads pressed/released correctly as a diagnostic input (not driven as an output).
- [ ] BOOT/RESET recovery sequence above actually restores flashability after a bad flash or an unresponsive image.

None of these boxes may be checked from simulator or native-test evidence; each requires the physical board. See the Evidence sections of `docs/tickets/F05.md`, `F06.md`, `F07.md` for what has and has not been exercised so far.
