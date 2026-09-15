<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Device lifecycle

## Pairing

Each Monitor Device and Monitored Host creates a random persistent Pairing Identity. On the first valid physical-USB handshake, the display and Host CLI or setup application show the Host label and a matching short code. Touch confirmation binds the two identities. The code catches accidental selection of the wrong attached monitor; the trusted physical USB boundary remains the security boundary.

MVP allows one Monitor per Host and one Host per Monitor. The CLI can enumerate other compatible devices but does not exchange Host data with them. Unpairing on either side invalidates the peer relationship, stops Host data exchange, and leaves both histories intact. The Monitor immediately continues standalone radio operation while power remains available.

## Connection recovery

Every attachment, daemon restart, or wake creates a fresh protocol session. Both sides exchange identities, versions, capabilities, configuration revisions, and current state. The Host sends an immediate complete snapshot after negotiation and continues sending complete snapshots every second. MVP uses no incremental Host Metric updates. Commands include identities and sequence numbers and are safe to retry. Radio observation, Alert evaluation, and device persistence continue without Host software or a data connection only while power remains available. The normal USB cable supplies power; removing that sole supply stops monitoring. Uninterrupted operation across physical unplugging is not an MVP requirement.

## Time

The board has no dependable battery-backed wall clock. The Host sends UTC, timezone, and uncertainty at handshake and periodically afterward. Before the first valid source after cold boot, the display says `Time unavailable`; it does not invent a date. Firmware uses monotonic time within each boot and stores resolved UTC, source, and uncertainty when available. M2 specifies a boot identity and durable record-ordering scheme so monotonic clock restarts do not reorder retained evidence. UTC remains optional, and the duration of an unpowered interval is unknown without a trustworthy time source.

A significant correction records a Time Discontinuity rather than rewriting existing records. Retained Host-impact evidence includes source age and timing uncertainty; uncertain timing cannot establish causation. Extreme Severity is deferred, and its eventual timing/uncertainty criteria require validation before that category can ship.

## Alert interruption

Coverage Gaps pause episode evaluation and observed-quiet recovery. On reboot, firmware preserves committed evidence and marks previously active episodes interrupted. The record keeps the last observed evidence and notes journal recovery on this boot without inventing an exact power-loss time or a quiet interval. Fresh qualifying evidence starts new episodes. This interruption state is distinct from observed recovery and does not itself create a suspicious-radio Alert; concrete persistence damage remains an Operational Alert.

## Recovery and removal

The 4 MB profile keeps one application slot. The updater validates the signed release manifest and image checksum before writing, then verifies the running version. If an update is interrupted or does not boot, the Operator uses the ESP32 ROM serial bootloader and documented BOOT/RESET recovery.

Removing Host software, deleting Host History/configuration, removing the V2 privileged helper, unpairing, erasing Radio History, and factory-resetting the Monitor are distinct operations. No uninstall action silently crosses those ownership boundaries.
