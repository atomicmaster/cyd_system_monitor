<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Isolate hardware support in profiles

Board descriptions, pin assignments, vendor references, development setup, and generated hardware configuration will live under independent hardware profiles. The profile describes LCDWiki's 2.8-inch E32R28T/E32N28T family, while the MVP support claim covers only the connected and verified E32R28T touch variant. Product behavior consumes declared capabilities such as display geometry and touch availability. This boundary costs some indirection now but prevents a future round display or another ESP32 board from scattering hardware conditionals through the firmware.
