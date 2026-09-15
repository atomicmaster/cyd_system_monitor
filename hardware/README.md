<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Hardware profiles

Each supported Monitor Device has an isolated directory under `profiles/`. A profile owns the board description, pin assignments, vendor source material, development notes, and any generated setup files needed to build and flash that hardware.

Application behavior should depend on profile capabilities rather than board names. This allows a future profile, such as a round display, to supply different display geometry, input hardware, storage, and pins without changing the domain model.

## Profiles

- [`lcdwiki-esp32-32e-2.8`](profiles/lcdwiki-esp32-32e-2.8/) — profile family for LCDWiki E32R28T/E32N28T; only the verified E32R28T touch variant is supported by the MVP
