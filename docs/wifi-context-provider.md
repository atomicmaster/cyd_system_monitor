<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# WiFi context provider IPC

The contract between the Host daemon (TB05 implements the daemon side) and the
signed SwiftUI setup application or a helper it owns (TB08 implements the
provider side). It carries the optional **Association Context**: the current
WiFi band/channel and, when macOS allows it, the SSID and BSSID. See
[ADR 0018](adr/0018-make-wifi-association-context-optional.md),
[radio model](radio-model.md), [macOS components](macos-components.md) and the
wire group in [`protocol/CORE_PROTOCOL.md`](../protocol/CORE_PROTOCOL.md)
(Network, key 9) that the daemon feeds from it.

## Roles

- The **daemon** owns the USB serial port and the one-second snapshot loop. It
  never calls CoreWLAN or CoreLocation and never asks for permission. It
  measures traffic and reads Host Station Identity itself, so those never
  depend on a provider.
- The **provider** is the only process that may call CoreWLAN or request
  Location Services. It sends what it learns; it never receives Host Metrics.
- No coordinates, place names, movement or Location authorization tokens are
  ever exchanged. The only Location-related fact is the permission *status*.

## Transport

- A Unix domain stream socket `wifi-context.sock`, owned by the daemon, in the
  per-user run directory: `$CYD_RUN_DIR` when set (development and tests), else
  `~/Library/Application Support/CYD System Monitor/run/`. The path plus NUL
  must fit `sun_path` (104 bytes); the daemon refuses to start the server
  otherwise and continues without it.
- The directory is created `0700` (an existing one is tightened to `0700`; a
  symlink, a non-directory or a directory owned by another user is refused). The
  socket is `0600`. A leftover socket from a crashed daemon is removed only if
  nothing answers on it; a live owner or any other kind of file at the path
  makes the daemon leave it alone.
- The daemon verifies the peer with `getpeereid`: a connection whose effective
  uid is not the daemon's own is closed without a byte read or written and
  counted (`wrong_uid`).
- One provider at a time. A newer same-uid connection **replaces** the older
  one (the older is closed and counted as `replaced`), so a restarted provider
  does not have to wait for the daemon to notice its dead predecessor.

## Framing: JSON lines

One JSON object per line, UTF-8, terminated by `\n`, at most **512 bytes**
excluding the newline. A longer line is discarded to its newline without being
buffered and counted (`oversized`).

Why JSON lines and not the length-prefixed CBOR of the USB protocol: the
provider is Swift, where `JSONEncoder` and a `FileHandle` line reader are
built in, while a CBOR encoder would be new code on the side that Operators
cannot easily debug. The message set is tiny and at most a few per second, so
size is irrelevant, and a socket log stays readable. The daemon accepts only a
**flat** object (no nesting, no arrays); values are strings without escape
sequences, canonical integers (no leading zeros, no fractions or exponents, at
most 15 digits), `true`/`false` and `null`; duplicate keys are rejected; at most
16 fields. Unknown extra keys are ignored, so version 1 can grow additively.
Anything else is a malformed line (counted, ignored). Sixteen malformed lines in a
row close the connection as a broken provider.

Every message carries `"v": 1` and `"type"`. A `v` other than 1 on any message
is answered with `{"v":1,"type":"error","reason":"unsupported_version"}` and the
connection is closed (`bad_version`).

## Messages

Provider to daemon:

| `type` | Fields | Notes |
| --- | --- | --- |
| `hello` | `provider` (1-32 of `A-Za-z0-9._- `), `provider_version` (same) | First line on every connection. Data before `hello`, or a second `hello`, is malformed. |
| `permission_status` | `seq`, `status`: `not_requested`, `denied`, `restricted`, `granted`, `unsupported`, `error` | Sent whenever the Location permission changes, and once right after `hello`. `granted` alone says nothing about whether SSID/BSSID are readable. |
| `context` | `seq`, `t_ms`, `age_ms`, `availability`, `band`, `channel`, `ssid_hex`, `bssid` | The current association. All nine fields required (use `null` for unknown). |

`context` fields:

- `seq`: integer 0..4294967295, strictly increasing across **all** messages of a
  connection (`permission_status` shares the counter).
- `t_ms`: the provider's monotonic clock in milliseconds when the sample was
  taken (0..2^53); must not decrease. Used only for ordering; the daemon never
  compares it with its own clock.
- `age_ms`: how old the underlying CoreWLAN sample is when the line is sent
  (0..4294967295), normally 0. The daemon reports
  `age = age_ms + time since it received the line`.
- `availability`: the shared **Association Context Availability** (CONTEXT.md):
  `available`, `not_requested`, `denied`, `restricted`,
  `authorized_unavailable`, `unsupported`, `error`. Permission and data
  availability are separate: `authorized_unavailable` is a granted permission
  that still yields no SSID/BSSID.
- `band`: `"2.4"`, `"5"`, `"6"` or `null`. `channel`: integer 1..233 or `null`.
  Band and channel need no Location permission, so they may accompany any
  availability. Send what CoreWLAN reports; do not infer a band from a channel.
- `ssid_hex`: the SSID octets as lowercase or uppercase hex, at most 64 hex
  digits (32 octets), or `null`. Hex because an SSID is arbitrary octets, not
  necessarily UTF-8, and must not need JSON escapes. `bssid`: `"aa:bb:cc:dd:ee:ff"`
  or `null`. **Both must be `null` unless `availability` is `available`**; a
  message that carries them otherwise is malformed and dropped.

Daemon to provider (informational; the provider must not depend on more than
receiving the ack before sending data):

| `type` | Fields |
| --- | --- |
| `hello_ack` | `heartbeat_max_ms` (1000), `silence_limit_ms` (15000) |
| `error` | `reason` (`unsupported_version`); the connection is then closed |

## Rate

The provider sends a `context` on every change (association, band/channel,
permission, availability) **and a heartbeat `context` at least once per second**
while connected, even if nothing changed. The heartbeat is what keeps the
context fresh for the display. The daemon accepts at most 20 lines per second
per connection and drops the excess (`rate_limited`); a change-driven message
plus one heartbeat per second is far below that.

Drops are counted, never silent: `malformed`, `oversized`, `out_of_order`
(a `seq` or `t_ms` that went backwards), `rate_limited`, `wrong_uid`,
`bad_version`, `replaced`, `closed_for_bad_lines`. The foreground `cyd-host run`
prints them when any is non-zero, and `status` reports them
([host-control-ipc.md](host-control-ipc.md)) for the setup UI.

## What the daemon reports

| Situation | `association_context_availability` sent to the display | Band/channel/SSID/BSSID |
| --- | --- | --- |
| No provider has ever connected | `not requested` | none |
| Connected, only `hello` and a `permission_status` so far | the status (`granted` reads `authorized but unavailable`) | none |
| A `context` newer than 15 s | the message's `availability` | as sent (SSID/BSSID only with `available`), with true age |
| `permission_status` changes | the new status **immediately**; a revoked permission drops SSID/BSSID at once | band/channel kept |
| No `context` for 15 s (provider gone, hung or asleep), connected or not | the last permission status if it was `not requested`, `denied`, `restricted` or `unsupported`; otherwise `error` | all dropped |

Rules that hold in every row: **a stale value is never presented as fresh**;
the context half's own source age travels in the Network group and the device
marks it stale at 15 s; the daemon also refuses a provider's band/channel/SSID
when the Host has no active WiFi interface; and the daemon keeps monitoring,
sending traffic rates and Host Station Identity, whatever the provider does or
whether it exists. Matching SSID alone never establishes Host-targeted evidence
(radio model): the display and any rule consuming this data treat SSID and
BSSID as optional context only, and 5/6 GHz coverage means the board cannot
capture the Host's traffic whatever the SSID says.

The 15 s limit equals the device's own staleness limit for this half
(`kNetworkContextStaleMs`), so a context that the daemon still reports has not
yet been dimmed by the device, and the daemon's downgrade lands as a fresh
state one snapshot later.

## Restart semantics

- **Daemon restarts:** the socket disappears and the provider's connection
  drops. The provider reconnects with a delay of about 1 s (retrying until it
  succeeds), sends `hello`, then `permission_status`, then a `context`
  immediately. The daemon holds no provider state across its own restarts, so
  until then it reports `not requested`.
- **Provider restarts or crashes:** the daemon notices the closed connection (or
  the 15 s silence) and degrades as in the table above; the new provider's
  `hello` replaces any old connection. `seq` and `t_ms` restart at each
  connection.
- **Sleep and wake:** the provider sends a fresh `context` on wake; the daemon
  ages everything by its monotonic clock, so a context that slept for more than
  15 s is dropped rather than shown.

## What the Swift side must send

1. Connect to the socket path above; if it does not exist or refuses, retry
   about once per second and keep the app fully usable. The daemon's absence is
   not an error the Operator has to fix.
2. Send `hello` first, wait for `hello_ack`.
3. Send `permission_status` with the current Location authorization mapped to
   `not_requested` (`notDetermined`), `denied`, `restricted`, `granted`
   (`authorizedAlways`/`authorizedWhenInUse`), and again on every change
   (`locationManagerDidChangeAuthorization`). Use `unsupported` if the platform
   cannot provide it and `error` for failures.
4. Send `context` whenever the association, channel, band or permission changes
   and every second while connected. Set `availability` to `available` only if
   CoreWLAN returned an SSID or BSSID; if permission is granted and it returned
   neither, send `authorized_unavailable`; use the permission states otherwise.
   Send band and channel whenever CoreWLAN gives them, with SSID/BSSID `null`
   when not `available`.
5. Never send coordinates, location objects, place names, timestamps of
   movement, or anything else; never send a value the daemon did not ask for.
6. Keep every line at most 512 bytes and `seq` strictly increasing, `t_ms`
   non-decreasing, per connection.

## Implementation and evidence

- Parser, tracker and scripted provider: `host/metrics/src/wifi_context.rs`
  (`parse_provider_message`, `ContextTracker`, `FixtureProvider`); the
  `WifiContextProvider` trait is the seam the network collector reads.
- Socket server: `host/platform-macos/src/wifi_context_server.rs`
  (`WifiContextServer`), hosted by the daemon (`host/hostcore/src/daemon.rs`) and the
  foreground `cyd-host run`; the Swift provider is
  `macos/setup-app/Sources/SetupCore/{ProviderProtocol,ProviderRunner}.swift`.
- Tests: `cargo test -p metrics wifi_context` and
  `cargo test -p platform-macos wifi_context_server` (temp-dir sockets, wrong
  uid, oversized, malformed, fuzz-style, replacement, stale socket, mode bits).
- The Swift provider client and its wire tests are in `macos/setup-app`
  (`swift test`). The real granted/denied integration with macOS Location is
  TB08's gate, recorded jointly with TB05 and R03.
