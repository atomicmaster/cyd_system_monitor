<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Trust the physical USB boundary

The personal desk MVP will trust physical USB attachment and macOS device permissions, validate all protocol input, and omit application-layer encryption and cryptographic pairing. On first connection, the Monitor Device and Host show the Host label and matching short code, require touch confirmation, and retain random Pairing Identities. This prevents accidental selection among attached monitors but does not claim to authenticate a hostile Host. Pairing secrets would introduce provisioning and recovery flows without protecting against a compromised Monitored Host, which already controls displayed host data and device configuration. A future network transport must define a new trust model rather than inherit this decision.

MVP permits one paired Monitor per Host and one Monitored Host per Monitor. The CLI can list all attached compatible monitors, while replacement requires explicit unpairing. Hostname, USB path, and CH340 topology are descriptive discovery data rather than identity.
