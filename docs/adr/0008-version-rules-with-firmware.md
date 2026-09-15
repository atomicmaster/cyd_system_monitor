<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Version alert rules with firmware

Radio alert logic will ship as part of versioned firmware because the standalone Monitor Device owns evidence interpretation and Alert creation. Safe numeric thresholds may be configured over USB and reset to documented defaults, but the MVP will not execute downloaded rule scripts. This keeps rule behavior compatible with the radio parsers and persistence schema while allowing the Operator to tune noisy thresholds without reflashing.
