<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Use a portable host metric contract

The daemon-to-device protocol will define stable, platform-neutral Host Metrics rather than expose native operating-system counters. Each platform adapter will supply a value or an explicit Metric Availability state: unsupported, absent, temporarily unavailable, or error. This lets the display and protocol remain stable as Linux and Windows adapters are added and as optional hardware such as GPUs, NPUs, and batteries varies between hosts.
