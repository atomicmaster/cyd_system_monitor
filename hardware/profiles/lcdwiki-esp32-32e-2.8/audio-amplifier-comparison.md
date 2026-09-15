<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Audio amplifier comparison

LCDWiki's schematic labels the fitted amplifier `SC8002B`, while its product page links an `FM8002E` datasheet. Both datasheets describe a mono bridge-tied-load audio amplifier with external gain resistors, no output coupling capacitor, an active-high shutdown input, and the same SOP-8 pinout:

| Pin | SC8002B | FM8002E |
|---|---|---|
| 1 | shutdown | shutdown |
| 2 | bypass | bypass |
| 3 | positive input | positive input |
| 4 | negative input | negative input |
| 5 | output 1 | output 1 |
| 6 | supply | supply |
| 7 | ground | ground |
| 8 | output 2 | output 2 |

The profile's firmware behavior is therefore the same for either part: GPIO4 low enables the amplifier and GPIO4 high shuts it down; GPIO26 supplies the DAC signal through the board's fixed analog network.

| Published characteristic | SC8002B | FM8002E |
|---|---:|---:|
| Operating supply | 2.0–5.5 V | 1.6–6.0 V |
| Typical quiescent current | 6.5 mA | 4.4 mA, no load |
| Typical shutdown current | 0.6 µA | 4.2 µA |
| 8 Ω output at 1% THD+N | 1.2 W | 1.1 W |
| 8 Ω output at 10% THD+N | 1.5 W | 1.6 W |

These similarities make the FM8002E datasheet useful for understanding the circuit's firmware-facing behavior. They do not establish that the parts are qualified drop-in substitutes across all loads, temperatures, distortion limits, or board revisions. Hardware documentation and repair decisions should continue to name SC8002B unless inspection proves a different fitted part.

Sources: the vendor schematic and locally cached SC8002B and FM8002E manufacturer datasheets under `vendor/`.
