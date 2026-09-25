<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Host control IPC

The local status and control channel of the per-user Host daemon (`cyd-hostd`,
[TB08](tickets/TB08.md)). The CLI (`cyd-host`) and the setup application are its
clients; neither owns the USB serial port, the daemon does. The optional
WiFi-context channel is a separate socket with its own contract
([wifi-context-provider.md](wifi-context-provider.md)). Implementation:
`host/hostcore/src/control.rs` (server, parser, counters) and
`host/hostcore/src/daemon.rs` (what each command does); Swift client and models:
`macos/setup-app/Sources/SetupCore/ControlModels.swift`.

## Transport and access

- Unix domain stream socket `control.sock` in the per-user run directory:
  `$CYD_RUN_DIR` when set (development and tests), else
  `~/Library/Application Support/CYD System Monitor/run/`. The directory is
  `0700` (created, or tightened when it exists; a symlink, a non-directory or
  another user's directory is refused); the socket is `0600`. The lock file
  `daemon.lock` (below) lives in the same directory.
- The daemon verifies each peer with `getpeereid`; a connection whose effective
  uid is not the daemon's is closed without a byte read or written and counted
  (`wrong_uid`). The Swift client refuses a directory or socket that is not
  private to its own uid before connecting, and verifies the peer uid after.
- At most 8 concurrent connections (`busy` beyond that). Each request line must
  arrive complete within 10 s of the connection opening or of the previous
  reply; otherwise the connection is closed. The deadline is absolute (bytes
  trickling in do not extend it), so slow clients cannot hold every slot.
  Clients may use one connection per request (the setup application does) or
  several requests on one.
- A leftover socket from a crashed daemon is removed only if nothing answers on
  it. The path plus NUL must fit `sun_path` (104 bytes); otherwise the daemon
  runs without the control socket and logs it.

## Framing and versioning

One JSON object per line, UTF-8, `\n`-terminated, each way.

- **Requests** are flat objects of at most **512 bytes**: `"v"` (integer, must be
  `1`), `"cmd"` (string) and, for `pair_start` only, an optional `"port"`
  (`/dev/` plus at most 64 characters of `A-Za-z0-9._-`). Strings carry no escape
  sequences, keys are unique, at most 16 fields. Both sockets use the same
  parser (`metrics::flat_json::parse_flat_object`) and the same socket setup,
  line bound and peer check (`platform_macos::ipc`), so they accept the same
  subset.
- **Responses** are at most **8192 bytes**. Every response carries `"v":1` and
  `"ok"`. Success: `{"v":1,"ok":true,"cmd":"<cmd>"}` plus one payload key for
  `status` and `monitors`. Failure: `{"v":1,"ok":false,"error":"<code>","message":"<text>"}`.
- A request with a `v` other than 1 is answered `unsupported_version` and the
  connection is closed. Unknown request keys are ignored, unknown response keys
  must be ignored by clients: version 1 grows additively (a breaking change
  bumps `v`).

Nothing malformed is silent: each of these is dropped without effect and counted
(`counters.control`), and the client gets an error line where it can:

| Counter | Cause | Reply |
| --- | --- | --- |
| `malformed` | not valid JSON of the accepted subset, missing `v`/`cmd`, bad `port` | `malformed` |
| `oversized` | a line over 512 bytes (discarded to its newline, never buffered) | `oversized` |
| `bad_version` | `v` not 1 | `unsupported_version`, then closed |
| `unknown_command` | valid request, no such command | `unknown_command` |
| `wrong_uid` | peer uid differs | closed, nothing sent |
| `busy` | more than 8 connections | closed |
| `closed_for_bad_lines` | 16 bad lines in a row | closed |

## Commands

| `cmd` | Fields | Effect |
| --- | --- | --- |
| `status` | | Returns the status object below. Answered from shared state, so it works during a 75 s pairing wait. |
| `monitors` | | `"monitors":[{"port","pairing_identity","state"}]`: candidate serial devices (`/dev/cu.usbserial*`, `usbmodem*`, `wchusbserial*`, `SLAB_USBtoUART`). A Pairing Identity is learned only by handshaking, so it is filled in only for the device this daemon is currently paired with; other entries have `null` and `"unknown"`. |
| `pair_start` | optional `port` | Starts pairing with the attached display. Refused with `already_paired` if a display is paired (unpair first). The daemon opens the port, negotiates, and reports `pairing_required` with a six-digit `pairing_code`. **The confirmation is the touch on the display**, which shows the same code; the protocol has no Host-side confirmation step, so there is no `pair_confirm` command. |
| `pair_cancel` | | Abandons a pairing attempt in progress. |
| `unpair` | | Sends the Unpair command to the connected paired display and, when it applies it, forgets the stored display on this Mac. `not_connected` when no paired display is connected. Histories and the Host identity are untouched. The daemon does not reconnect until `pair_start`. |
| `reset_calibration` | | Sends the calibration-reset command to the connected display (guided calibration on restart). |
| `forget` | | Forgets the stored display on this Mac only, without contacting it (the display keeps its pairing). Refused while connected (`paired_connected`): use `unpair`. |
| `stop` | | Clean stop: the daemon exits with status 0, which the LaunchAgent does not restart. |

Commands that talk to the display (`unpair`, `reset_calibration`) wait up to 20 s
for its reply and may return `rejected`, `link_error` or `timeout`.

An unpaired display that is merely plugged in is never touched: the daemon
reports `pairing_required` and waits for `pair_start`. A refusal (another Mac
owns the display, protocol mismatch, declined or timed-out pairing) parks the
port: it is not retried, because opening a port resets the ESP32, until the
display is replugged or the Operator acts.

## Status object

```json
{"v":1,"ok":true,"cmd":"status","status":{
  "daemon":{"version":"0.0.0","pid":4242,"uptime_s":90,"uid":501},
  "link":{"state":"paired_connected","detail":null,"port":"/dev/cu.usbserial-10",
          "pairing_identity":"d0d0d0d0","pairing_code":null,"sessions":2,"snapshots":88,
          "last_snapshot_utc_ms":1790205982430,"last_snapshot_age_ms":412},
  "metrics":{"cpu":"available","ram":"available","storage":"available","battery":"absent",
             "agents":"available","network":"available"},
  "wifi_context":{"provider":"connected","availability":"authorized_unavailable","permission":"granted"},
  "counters":{"provider":{"accepted":1,"replaced":0,"wrong_uid":0,"malformed":0,"oversized":0,
                          "out_of_order":0,"rate_limited":0,"bad_version":0,
                          "closed_for_bad_lines":0,"applied":30},
              "control":{"accepted":9,"requests":9,"wrong_uid":0,"malformed":0,"oversized":0,
                         "bad_version":0,"unknown_command":0,"busy":0,"closed_for_bad_lines":0}}}}
```

- `link.state`: `no_device` (nothing attached), `device_found` (attached or
  connecting; `detail` says why it is not further, such as several candidate
  devices or an incompatible protocol), `pairing_required` (attached but not
  paired, waiting for `pair_start`, or a pairing in progress with a
  `pairing_code`, or refused), `paired_connected` (handshaken; a complete
  snapshot goes out every second), `stale` (a paired display's link was lost or
  is resynchronizing after sleep/wake; retrying). `detail` is human text,
  never an address. `port` is the device node. `pairing_identity` is the first
  four bytes of the stored display's Pairing Identity as hex (`null` when none);
  the full identity and the Host identity are never reported.
- `link.sessions` counts connection attempts that opened the port: it rises by
  one on each reconnect, replug or sleep/wake resynchronization. `snapshots`
  counts snapshots sent by this daemon. `last_snapshot_utc_ms` /
  `last_snapshot_age_ms` are `null` before the first.
- `metrics`: availability of each group in the latest snapshot
  (`available`, `unsupported`, `absent`, `temporarily_unavailable`, `error`;
  storage and battery report the less healthy of their two halves). It is empty
  unless `paired_connected`; a stale summary is never shown.
- `wifi_context`: `provider` is `never_connected`, `connected`, `disconnected` or
  `server_unavailable` (the socket could not start). `availability` is the
  Association Context Availability the daemon would report **now**, computed
  from the tracker, so it degrades to `error` (or the last refusal state) 15 s
  after the provider goes silent or away, whether or not a display is attached.
  `permission` is the permission status **as last reported by the provider**
  (`not_requested`, `denied`, `restricted`, `granted`, `unsupported`, `error`),
  `null` if it never spoke. The daemon does not check Location itself.
- `counters.provider`: the provider socket's drop counters, `null` when its
  server did not start. `counters.control`: this socket's, table above.

## Single ownership of the serial port

The daemon (or a foreground `cyd-host run`, `unpair` or `reset-calibration`
that opens the port itself) holds an exclusive `flock` on
`<run dir>/daemon.lock` for its lifetime, and writes its pid into it. The kernel
releases it when the holder dies, so a crash never leaves a stale lock. A second
holder refuses with the first's pid:

> another cyd-host process (pid N) already owns the display's serial port; use
> `cyd-host status` to see it or `cyd-host service stop` to stop it

While the daemon runs, `cyd-host unpair`, `reset-calibration` and `forget` are
forwarded to it over this socket and never open the port; `cyd-host run`
refuses. The daemon exits with a non-zero status for a refused start; launchd's
throttle interval limits any respawn.

## Restart, sleep and wake

- The daemon holds no pairing state beyond the identity file, so a restart
  reconnects a paired display with a fresh protocol session and, as soon as the
  handshake completes, sends a complete snapshot, then one per second. The
  status is `paired_connected` again once that happens.
- Unplug and replug: the write or read error moves the state to `stale`, then
  `no_device`; on replug the daemon reconnects the same way, without a touch
  (the display remembers the Host).
- Sleep and wake: a gap of more than 5 s between two ticks (on the wall clock
  or on the daemon's monotonic clock, which keeps counting while the Mac
  sleeps) ends the session and reconnects with a fresh session and an
  immediate snapshot, whether or not the USB link reported an error. Every
  sample age is measured on that monotonic clock, so a reading from before the
  sleep is as old as the sleep. On wake every rate re-baselines (the first
  post-wake rate is `temporarily_unavailable`) and battery charge, storage
  capacity, agent presence and the network view are read again before the
  next snapshot; a re-read that fails keeps the old value with its true age,
  which the display shows stale. A provider context older than 15 s is
  dropped.
- A provider, a permission or the setup application being absent never stops the
  daemon or its snapshots.

## Evidence

- `cargo test -p hostcore control` (parser, counters, wrong uid, oversized,
  flood, modes; temp-dir sockets) and `cargo test -p hostcore --test daemon`
  (the daemon against the real firmware `HostLink` scripted device: status
  through every state, second-instance refusal, provider degradation after 15 s
  with an injectable clock, status clients closing and reopening, unplug/replug
  and sleep/wake resynchronization, unpair, stop).
- `swift test` in `macos/setup-app` decodes `Fixtures/status-no-device.json`
  (captured from a real `cyd-hostd` run) and `status-paired.json`.
