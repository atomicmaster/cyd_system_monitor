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
