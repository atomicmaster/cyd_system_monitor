<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Use versioned CBOR over USB

The daemon and firmware will exchange length-prefixed CBOR messages containing a protocol version, message type, sequence number, timestamps, and an integrity check. Raw packed structs would couple both sides to compiler layout, while newline JSON would spend more of the ESP32's memory and serial bandwidth as the contract grows. A human-readable schema and desktop decoder will keep the binary protocol inspectable and replayable in tests.

Every USB connection creates a fresh negotiated session. Peers exchange Pairing Identities, capabilities, supported versions, current configuration revisions, and time uncertainty before the Host sends an immediate complete snapshot and then complete snapshots every second. MVP uses no incremental Host Metric updates; this keeps reconnect and stale-state handling simple. Reconsider deltas only if measured bandwidth justifies the added complexity. Commands carry identities and sequence numbers so retries are idempotent. A cable removal, daemon restart, or Host sleep cannot make stale messages apply to a new session.
