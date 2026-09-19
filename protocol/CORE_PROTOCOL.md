<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# M2 USB protocol core

Frames are bounded to 2,048 bytes: a two-byte big-endian frame length,
canonical CBOR, then a big-endian IEEE CRC-32 over the CBOR bytes. The length
includes CBOR and CRC but not itself. A bad length, CRC, duplicate known key,
unknown required value, unsupported major, or nesting beyond the decoder limit
is rejected without allocation beyond the frame limit.

The CBOR envelope is an integer-key map: `0` major, `1` minor, `2` message
type, `3` 16-byte session identity, `4` sequence, `5` body, optional `6` UTC
milliseconds, and optional `7` UTC uncertainty in milliseconds. Types are
Hello, HelloReply, Pair, complete Snapshot, Command, CommandReply, and Error.
UTC fields use canonical CBOR unsigned 64-bit integers when needed; they are
correlation/display context, not an ordering key. Unknown envelope keys are
optional and skipped as bounded CBOR values. Version major 1 is supported; a
different major produces `unsupported-major` and must not apply metrics or
configuration.

Every negotiated connection has a new random Pairing Identity/session identity.
The receiver accepts monotonically increasing sequences only within that
session. Commands include their sequence and must cache the reply for an
idempotent retry. Pair and command bodies are intentionally opaque in this
foundation; their maximum body is 512 bytes. The first concrete body is a
complete CPU snapshot (availability, utilization in per-mille, source age,
freshness, and configuration revision). There are no incremental metric
updates.

After Hello/HelloReply compatibility and Pair confirmation, the Host sends an
immediate complete snapshot and one complete snapshot per second. UTC is a
resolved display concern, not an ordering key: evidence ordering uses boot
identity plus monotonic sequence. A time correction records a Time
Discontinuity and never rewrites earlier evidence.

## Metric groups (C02)

The snapshot body is a field map. CPU keeps keys `0`-`4` exactly as before;
every further metric is an optional nested *group* map at a key `5` and up, so
a decoder that predates a group skips it as an unknown optional key and the
protocol stays 1.x. A group is all-or-nothing: when present, every field the
group defines is required, unknown extra keys inside it are skipped (same
bounds as anywhere), and a group appearing twice is malformed. The whole
snapshot still travels in one frame of at most 2,048 bytes.

| Key | Group | Fields |
| --- | --- | --- |
| 5 | RAM | `0` availability (0-4), `1` used bytes, `2` total bytes, `3` source age ms (32-bit), `4` freshness (0 fresh, 1 stale). Used is at most total; both are canonical unsigned 64-bit. |

Physical RAM Occupied is physical total minus free pages
(`docs/host-metrics.md`); the group carries the two byte counts, never a
percentage, so the display chooses the presentation.

## Link handshake and pairing (TB02)

The USB link is the board's UART bridge. Device output interleaves ASCII log
lines with frames; a log byte can never start a frame because a frame's first
byte (the high length byte) is at most `0x07`, so the host framer resyncs past
log text. Host bytes are frames only once the Host has sent the text line
`CYD:LINK` (which leaves the ASCII dev console for the negotiated link).

1. `Hello` (Host, session all-zero) carries supported version, capabilities,
   configuration revision, and Host UTC. The device answers `HelloReply` with
   its own supported version/capabilities/revision. A different major is
   answered with `Error` (body: one byte, `1` incompatible) and nothing is
   applied.
2. `Pair` (Host, session all-zero) carries the Host identity, the intended
   device identity (all-zero on first pairing), and the matching code (six
   digits, chosen by the Host and shown in its terminal). An unpaired device
   shows the code and waits for touch confirmation; a paired device accepts a
   `Pair` whose Host identity matches the stored one without a touch and
   answers `Error` (body byte `2`, not paired with you) to any other Host.
   Declined or timed-out confirmation answers `Error` (body byte `3`).
3. On acceptance the device answers `Pair` (its identity, the Host identity,
   the same code) whose envelope session is the fresh session identity the
   Host chose in its own `Pair` envelope. Both sides then use that session
   with sequence restarting at one. Snapshots for any other session, or sent
   before a session exists, are rejected.
4. Unpair is a `Command` (operation `1`) inside a session; the device forgets
   the Host identity, closes the session, and replies. Retained state stays.

## Control bodies

Control-message bodies are canonical CBOR maps; they remain bounded by the
512-byte non-snapshot body limit. `Hello` and `HelloReply` carry `0` supported
major, `1` supported minor, `2` capability bits, `3` configuration revision,
and optional `4` UTC milliseconds / `5` UTC uncertainty milliseconds. A peer
with an incompatible major replies with `Error` rather than applying a body.

`Pair` carries `0` the sender's 16-byte persistent Pairing Identity, `1` the
intended peer identity (or an all-zero identity on the first display of the
matching code), and `2` the six-digit matching code. It is confirmation data,
not a secret or cryptographic credential. A successful confirmation starts a
fresh session identity and restarts its sequence at one.

`Command` and `CommandReply` carry `0` command identity and `1` operation.
The first operations are `1` unpair and `2` calibration reset. A reply repeats
the command identity and includes `2` result (`0` applied, `1` already
applied, `2` rejected). Receivers cache the bounded reply by session and
command identity so a retry is idempotent. Unpair never deletes retained radio
history or unrelated settings.

The shared reducer makes a retained metric stale after 3 seconds and the Host
disconnected after 15 seconds. Adapter enablement is separate from both metric
availability and freshness. Alert acknowledgements mark an episode seen only;
observed-quiet is the only normal end transition, while recovery after a boot
marks active episodes interrupted. Radio-rule numeric thresholds, minimum
Observed Time, evidence counts, Confidence, and quiet recovery eligibility
remain intentionally uncommitted until M5/M6 measurements.

## Stream framing and resynchronization

Both codecs provide the same `FrameStream`. Bytes may arrive in any
fragmentation. At the buffer start it reads the length: a length under 5 or
over 2,046 discards one byte; an incomplete frame waits; a complete frame with
a bad CRC discards one byte. A frame with a valid length and CRC but a rejected
body (unsupported major, malformed CBOR, wrong body) is consumed whole and its
error is reported, so a bad message never desynchronizes the stream. Resync
after corruption can be delayed while a false length waits for up to 2,046
bytes. Byte-count alone is not a good recovery bound, so the receiving adapter
also abandons a partial frame after 250 ms without new bytes (the Host writes
each frame in one burst): `DropLeadingByte()` discards one byte and the caller
rescans. `HostFeed::Tick` does this in firmware/simulator.

Unknown optional values are skipped only when they are unsigned/negative
integers, byte/text strings, or arrays/maps of at most 16 items nested fewer
than 4 deep. Tags, floats, and simple values are outside the profile and
malformed. Envelope and body map keys must be strictly increasing; a repeat or
reordering is malformed.

## Idempotency and session reset examples

`command-v1.hex` (command 42, calibration reset) and `command-reply-v1.hex`
(same identity, result `1` already applied) are the retry pair: a duplicate
command sequence in the same session is `stale-sequence` at the sequence gate,
and the receiver resends its cached reply rather than re-applying. A new
session (matching-code Pair confirmation) restarts sequences at one and
discards the old session's cached replies; `session-parity-matrix.txt` lists
accept, stale, wrong-session, and reset rows that both receivers run.

## Fixtures and matrices

`protocol/fixtures/parity-matrix.txt` lists every vector with its expected
outcome (`none`, `bad-crc`, `unsupported-major`, `malformed`, `truncated`,
`too-large`); C++ and Rust both run every row. The golden CPU vector is
produced by the codecs; the compatibility and malformed vectors are produced by
`protocol/tools/gen_vectors.py`, which assembles bytes independently of either
codec. `./dev run protocol -- decode <fixture>` inspects any of them.

## Decoding rules both codecs share

These were settled by the parity gate below; each has a fixture row in
`parity-matrix.txt` and a matching entry in the `any`/`cpu`/`control` decoders.

- **Order of rejection:** frame length/CRC, then the envelope map (at most 16
  entries, strictly increasing keys), evaluated in key order.
- **Major first:** major is envelope key 0. A value other than 1 is
  `unsupported-major` immediately; nothing after it is interpreted, because a
  future major may change the layout. A repeated or misordered key is
  `malformed` before any version error.
- **Message type before body:** an unknown message type is `malformed`
  whatever the body. A non-snapshot body over 512 bytes is `too-large`. A
  Snapshot body must be a field map with every CPU field present.
- **Unknown optional keys** are skipped in the envelope, the snapshot body, and
  every control body (keys above the type's last known key: Hello 5, Pair 2,
  Command 1, CommandReply 2). A skipped value is an integer, string, or
  array/map of at most 16 items; its nesting is counted from that value (a
  body's own map is not a level), so up to 3 arrays deep is accepted and 4 is
  `malformed`. Tags, floats, simple values, and indefinite lengths are outside
  the profile.
- **No trailing bytes** after the envelope map or a body.

## Protocol change gate

Any change to the C++ codec (`protocol/core/cpp/`), the Rust codec
(`host/protocol/`), the schema, or a fixture is not done until the two codecs
have been analyzed against each other:

1. Run `./dev check protocol` (also run by the firmware and host lanes).
   It ends with `protocol/tools/parity_diff.py`, which feeds one deterministic
   corpus (every fixture, byte-level and structure-aware mutations, extension
   entries with every CBOR value kind, and fragmented/damaged byte streams;
   about 180,000 inputs) to both codecs and compares the outcome, every decoded
   field, re-encoded bytes, and stream-resynchronization results.
2. Any `MISMATCH` is a misalignment. **Test first:** add the input as a
   fixture (`protocol/tools/gen_vectors.py`) with a row in `parity-matrix.txt`,
   and confirm it fails. Decide which side is wrong from this document (or
   amend this document with the decision), then fix the codec. Never fix a
   mismatch without its fixture.
3. A misalignment found by reading code rather than by the corpus gets the same
   treatment, and its mutation class should be added to the corpus generator so
   the gate can find its siblings.
4. Do not mark a protocol ticket or section Done until the gate reports
   `0 mismatch(es)`; record the summary line (inputs, seed, mismatches) and run
   at least two extra seeds (`./dev run protocol -- parity --seed 2`) in the
   ticket's handoff.
5. Behavior only one codec exercises (framing helpers, session gates) still
   needs a shared row in `session-parity-matrix.txt` or a `FrameStream` case in
   the corpus.

