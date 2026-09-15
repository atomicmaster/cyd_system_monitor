<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Release components independently with a compatibility manifest

Firmware, host workspace, macOS setup package, and protocol schema will have independent versions bundled beneath a signed product release manifest containing checksums and an explicit compatibility matrix. One shared version would imply lockstep compatibility and force unrelated components to release together, while completely separate artifacts would leave supported combinations ambiguous. The Monitor Device's About screen will expose the supported, incoming, and negotiated protocol versions for field diagnosis.
