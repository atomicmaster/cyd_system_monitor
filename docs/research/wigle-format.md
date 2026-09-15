<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# WiGLE-compatible wardrive export

Research date: 2026-09-15

This note defines the compatibility target for a future GPS-assisted wardriving feature. Statements under **Verified** come from WiGLE's current first-party documentation, API description, or Android client. Statements under **Recommendation** are project design choices and should be validated with an upload fixture before release.

## Compatibility target

**Verified.** WiGLE's current interchange format is **WiGLE Wireless CSV 1.6**. It is UTF-8 and mostly follows RFC 4180, with one deliberate extension: a CSV-escaped device pre-header precedes the ordinary column header. The format version is to be treated as fixed for the implemented version. [WiGLE CSV 1.6 specification](https://api.wigle.net/csvFormat-1_6.html)

The two opening records are:

```csv
WigleWifi-1.6,appRelease=<version>,model=<model>,release=<release>,device=<device>,display=<display>,board=<board>,brand=<brand>,star=Sol,body=3,subBody=0
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type
```

Earth is `star=Sol,body=3,subBody=0`. The remaining pre-header values describe the producer and hardware and are CSV fields, so values containing a comma or quote must be escaped normally. WiGLE's own client emits the pre-header through a CSV writer and then writes the exact fixed 14-column header shown above. [WiGLE CSV specification](https://api.wigle.net/csvFormat-1_6.html) [WiGLE Android `ObservationUploader`](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java#L68-L104)

**Recommendation.** Emit exactly `WigleWifi-1.6` and the exact ordered header. Use project firmware version for `appRelease`; stable board-profile identifiers for `model`, `device`, and `board`; ESP-IDF version for `release`; display profile for `display`; and the board/vendor name for `brand`. Do not add columns or proprietary header fields. Store any session metadata in a sidecar manifest or local index.

## Observation fields

**Verified.** Every data record uses all 14 column positions in the fixed order. A blank value still occupies its position. WiGLE documents these meanings: [WiGLE CSV 1.6 specification](https://api.wigle.net/csvFormat-1_6.html)

| Column | WiFi | Bluetooth / BLE | Presence and representation |
| --- | --- | --- | --- |
| `MAC` | BSSID | BD_ADDR | Hardware address; the format examples use colon-separated hexadecimal. |
| `SSID` | SSID | Published device name, when available | May be blank. It must be CSV-escaped and encoded as UTF-8. |
| `AuthMode` | Capabilities array, based on Android-style capability sets | Device-class/capability description, optionally identifying the first scan as BT or BLE | May be blank when the scanner cannot derive it. |
| `FirstSeen` | Observation timestamp | Observation timestamp | UTC, second precision, `YYYY-MM-DD hh:mm:ss`. |
| `Channel` | Integer channel | `0` | Required column; WiFi value may be blank when unknown. |
| `Frequency` | Integer center frequency in MHz | Bluetooth device-type code | May be blank when unknown; WiGLE's BLE example leaves it blank. |
| `RSSI` | Radio-reported RSSI | Radio-reported RSSI | Signed numeric value. |
| `CurrentLatitude` | Observation latitude | Observation latitude | Decimal degrees. |
| `CurrentLongitude` | Observation longitude | Observation longitude | Decimal degrees. |
| `AltitudeMeters` | Estimated position altitude | Estimated position altitude | Meters. |
| `AccuracyMeters` | Estimated position accuracy | Estimated position accuracy | Decimal meters. |
| `RCOIs` | Space-delimited Roaming Consortium Organization Identifiers | Blank | Optional value. |
| `MfgrId` | Blank | Bluetooth manufacturer ID, when available | Optional value. |
| `Type` | `WIFI` | `BT` or `BLE` | Record discriminator. |

The official Android client writes one row for every stored observation, preserves Unicode labels, formats the timestamp in UTC, and emits decimal numbers using the US locale without digit grouping. Its type enum confirms distinct `WIFI`, `BT`, and `BLE` values. [row writer](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java#L467-L589) [network types](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/model/NetworkType.java#L6-L18)

**Recommendation.** Retain raw over-the-air WiFi BSSIDs and Bluetooth addresses as requested. Represent unknown values as empty fields rather than invented zeros, except Bluetooth `Channel`, whose documented value is `0`. Sanitize malformed UTF-8 and rely on a real CSV encoder for commas, quotes, and line breaks in labels. Do not export observations without a usable position fix; retain them separately as ordinary radio history if useful.

## Encoding, line endings, names, and compression

**Verified.** The normative encoding is UTF-8 and the records are CSV-escaped. WiGLE says the format follows RFC 4180 "for the most part," but does not state a separate mandatory line-ending rule. Its current Android producer intentionally uses LF (`\n`) instead of RFC 4180's CRLF. [format specification](https://api.wigle.net/csvFormat-1_6.html) [Android CSV configuration](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java#L95-L104)

The API accepts a file as a multipart upload. It accepts one or more supported observation files inside ZIP, TAR, or TAR.GZ archives, ignores archive members after the first 200, and caps an uploaded file at 180 MiB. [WiGLE API 3.1 Swagger document](https://api.wigle.net/swagger.json) The current official Android client produces a single gzip stream named `WigleWifi_yyyyMMddHHmmss.csv.gz`, demonstrating first-party use of gzip-compressed CSV. [Android `FileAccess`](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/util/FileAccess.java#L30-L63)

No official source reviewed imposes a specific base filename. The API does require the multipart part to include both a filename and payload. [WiGLE API 3.1 Swagger document](https://api.wigle.net/swagger.json)

**Recommendation.** Write UTF-8 without a byte-order mark, use LF endings to match the official client, and finish each record with a newline. Create one gzip-compressed CSV per recording session with a sortable collision-resistant name such as `WigleWifi_20260915T184233Z_0001.csv.gz`. Keep every file below 180 MiB; rotate substantially earlier, for example at 128 MiB compressed, so interrupted transfers and future growth do not approach the service limit. Download the original `.csv.gz` bytes from the device rather than recompressing them on the Host.

## GPS fixes and time

**Verified.** `FirstSeen` is UTC at second precision. Latitude and longitude are decimal degrees, altitude is meters, and `AccuracyMeters` is the estimated position accuracy in meters. The format specification does not state a maximum acceptable accuracy, a minimum movement distance, a GPS age limit, or a required sampling cadence. [WiGLE CSV 1.6 specification](https://api.wigle.net/csvFormat-1_6.html)

**Recommendation.** Each radio row should use the closest valid GPS fix in monotonic time and record the fix's own accuracy. Reject a fix that is stale, invalid, or outside a configurable accuracy ceiling rather than recording `0,0`. Start with a conservative ceiling such as 50 m and a maximum fix age such as 5 seconds, record why observations were omitted, and tune these values through field testing. Keep UTC from GPS when valid; persist monotonic ordering and protect against wall-clock jumps. Because CSV only has second precision, deterministic row order should break same-second ties.

## Sessions, repeat observations, and deduplication

**Verified.** CSV 1.6 has no session ID or row ID. WiGLE's Android client can export the current run, the entire local database, or observations since its last-upload marker, but all variants serialize into the same schema. After a successful incremental upload, it advances a local observation-ID marker; failed uploads do not advance it. [WiGLE Android README](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/README.md#data-export) [upload marker handling](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java#L258-L303)

WiGLE describes its mapped position as a signal-strength-weighted centroid after clustering multiple submitted observations. That confirms repeated observations are useful inputs. WiGLE does not document a server-side duplicate key, retry idempotency guarantee, or exact deduplication policy in the reviewed format/API material. [WiGLE FAQ](https://wigle.net/faq)

**Recommendation.** Preserve repeated observations across a route and avoid deduplicating solely by MAC address. To control volume, coalesce only observations that are effectively identical within a short interval, and document that policy. Treat each closed `.csv.gz` as an immutable session artifact. Maintain a sidecar index with session ID, start/end UTC, row count, byte size, SHA-256, transfer state, and optional upload transaction ID. A Host download should be resumable and checksum-verified; it must not mark a file uploaded to WiGLE. Never regenerate a different payload under the same session identity.

## Upload and Host workflow

**Verified.** Programmatic upload is `POST https://api.wigle.net/api/v2/file/upload` with `multipart/form-data`; the required part is named `file`. Optional form field `donate=on` permits commercial use of the uploaded content. A successful response returns one or more transaction IDs, and authenticated users can query `/api/v2/file/transactions` for processing status. [WiGLE API 3.1 Swagger document](https://api.wigle.net/swagger.json) The official Android client sends the file as `application/octet-stream` and supports both account-associated and anonymous upload modes. [Android API client](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/net/WiGLEApiManager.java#L608-L670) [anonymous upload selection](https://github.com/wiglenet/wigle-wifi-wardriving/blob/754409273d3238b3887fd7ad55885aa2e519b974/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java#L223-L260)

**Recommendation.** The V2 Host application should list sessions on the SD card and let the operator download selected original WiGLE files. Do not auto-upload. A download action stays local and needs no WiGLE credential. Provide a shortcut to WiGLE's manual [CSV Upload page](https://wigle.net/uploads) and leave submission to the operator. If direct API upload is added later, require a separate explicit action, store the API name/token in the platform credential store, show the transaction status, retry only after checking local transaction state, and make commercial-use donation an explicit opt-in.

## Privacy and account implications

**Verified.** A wardrive file contains precise coordinates and times alongside SSIDs, WiFi BSSIDs, Bluetooth addresses, and sometimes Bluetooth names/manufacturer IDs. Uploading incorporates those observations into WiGLE's database. WiGLE says an uploader's raw observations and traces are directly available only to that uploader; public data exposes a trilaterated network location rather than the contributor's individual trace, and public network data does not name the first observer. WiGLE also provides a BSSID-based removal process. [WiGLE privacy FAQ](https://wigle.net/privacy)

WiGLE says an account requires a real email address plus username and password, while other identifying fields need not be real. Its individual data license permits downloaded WiGLE data for personal, research, or educational non-commercial use and forbids credential sharing. Those terms concern use of WiGLE's data and account; the optional upload `donate` flag separately grants commercial use of contributed points. [WiGLE FAQ](https://wigle.net/faq) [WiGLE individual data license](https://wigle.net/eula.html) [WiGLE API 3.1 Swagger document](https://api.wigle.net/swagger.json)

**Recommendation.** Treat SD and downloaded wardrive files as sensitive location history. Show an explicit disclosure before the first export/download and state that a later WiGLE upload shares radio identifiers, labels, timestamps, and coordinates with an external service. Keep files local until the operator exports them, never bundle WiGLE credentials into firmware, never select commercial donation by default, and provide deletion on both Device and Host. Raw MAC addresses are appropriate for the requested export, but their over-the-air visibility does not remove the privacy risk created by pairing them with a durable time-and-location trace.

## Release validation

Before calling the export compatible:

1. Generate WiFi and BLE fixtures containing commas, quotes, Unicode, hidden/blank names, unknown optional values, negative coordinates, and same-second observations.
2. Decompress and parse the output with an independent RFC 4180 parser; verify two header records, 14 fields per data row, UTF-8, UTC timestamps, and exact `Type` values.
3. Compare representative output against the current official Android client's shape.
4. Upload a small consented test fixture through WiGLE's normal upload flow, retain its transaction ID, and confirm successful parsing of both WiFi and BLE rows.
5. Verify SD rotation, power-loss recovery, immutable completed files, Host resume, SHA-256 validation, and deletion without ever uploading automatically.

