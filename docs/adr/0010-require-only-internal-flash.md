<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Require only internal flash for persistent state

The MVP will keep touch calibration, configuration, transmitter baselines, and bounded Radio History in wear-aware partitions within the connected unit's 4 MB flash. High-rate counters will remain volatile, and MicroSD will be optional rather than required for normal operation. This preserves one-cable appliance behavior while forcing explicit storage budgets and eviction instead of allowing history to grow without bound.
