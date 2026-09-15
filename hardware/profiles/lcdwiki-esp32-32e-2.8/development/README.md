<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Development environment

Project-owned setup, build, flash, serial-debugging, and hardware smoke-test instructions for this profile belong here. ESP-IDF C++ and LVGL are selected; exact toolchain versions and reproducible setup remain M0 outputs.

Relevant unmodified vendor guides are under [`../vendor/development/`](../vendor/development/). The vendor package also contains Windows-only flashing tools; these are reference material and are not the planned macOS development workflow.

The M1a combined feasibility gate must resolve the three separately wired display/touch/MicroSD SPI buses against the ESP32's two general-purpose SPI controllers; software-driven touch SPI is a candidate, not a settled implementation. Record the chosen arrangement and measured touch/display behavior alongside radio capture, USB traffic, RAM/flash headroom, journal space, and write budgets. See [IMPLEMENTATION_PLAN.md](../../../../IMPLEMENTATION_PLAN.md).

The hardware smoke-test workflow must verify the XPT2046 interrupt and SPI pins, display orientation, guided first-start calibration, calibration persistence, and the deliberate recalibration recovery path.
