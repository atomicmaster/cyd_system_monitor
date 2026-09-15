<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Host Metric contract for macOS MVP

The macOS adapter runs in the logged-in Operator's per-user LaunchAgent. Ordinary collection requires no root, Full Disk Access, Accessibility, or Screen Recording permission. Apple GPU/ANE Activity Diagnostics and the fixed-purpose privileged helper are deferred to V2 and are not MVP collection or installation requirements.

## Metrics

| Host Metric | Meaning | macOS source | Default cadence |
|---|---|---|---:|
| CPU use | Delta of user, nice, and system ticks relative to all CPU ticks | Public Mach `host_statistics(HOST_CPU_LOAD_INFO)` | 1 s |
| Physical RAM Occupied | Physical total minus free pages; includes reclaimable caches | Public `hw.memsize` and Mach `HOST_VM_INFO64` | 1 s |
| Root disk capacity | Capacity and available space of the filesystem containing `/`; APFS volumes are not summed | Public Foundation filesystem capacity API | 30 s |
| Root disk read/write rate | Delta of bytes read/written by the physical storage driver backing `/` | Public user-space IOKit block-storage statistics | 1 s |
| Interfaces and addresses | Up, running, non-loopback interfaces with usable addresses | Public BSD `getifaddrs` | 30 s or network change |
| Network RX/TX rate | Per-interface 64-bit byte-counter deltas; tunnels are never summed with their underlying interfaces | Darwin route sysctl `NET_RT_IFLIST2` | 1 s |
| Host Station Identity | Current `AF_LINK` address of the active WiFi interface, including macOS Private WiFi Address | Public BSD `getifaddrs` | 1 s or link change |
| Host WiFi band/channel | Fresh current association band/channel when available; supports observable/outside-supported-coverage/unknown presentation, never continuous-coverage claims | macOS WiFi adapter; capability and permission behavior verified during M3 | 1 s or association change |
| Current WiFi BSS | Optional SSID/BSSID context when the Operator grants macOS Location Services access | Public CoreWLAN | 1 s or association change |
| Battery charge/state | Current charge and charging state, or absent on hosts without a battery | Public IOPowerSources API | 30 s or power change |
| Battery condition | Best-effort maximum capacity and condition with source age | Defensive parsing of `system_profiler SPPowerDataType -json` | 1 h |
| Agent Activity | Exact same-user resolved-binary-path matches for Codex, Claude, and Grok Build; activity remains unknown without an adapter signal | `libproc` plus AppKit workspace APIs | 5 s |

## V2 Apple Activity Diagnostics

V2 adds optional GPU and Neural Engine Activity at a candidate five-second cadence through the restricted `powermetrics` helper in [ADR 0016](adr/0016-isolate-apple-activity-diagnostics.md). Supported samplers and fields must be verified on the V2 support matrix. Apple warns that these power estimates may be inaccurate and should not be compared between devices; the product must not convert them into universal utilization percentages. GPU/ANE fields use Metric Availability states and never block other collection.

## Rate behavior

Rates require two samples separated by monotonic elapsed time. The first sample, sleep/wake, hotplug, identity replacement, counter decrease, or parser reset clears the baseline and reports `temporarily unavailable`; none of these conditions represents a measured zero rate.

The daemon sends a complete Host Metric snapshot every second and an immediate complete snapshot after fresh-session negotiation, including reconnect. MVP uses no incremental updates. Slow fields retain their own sample timestamps and become stale after three expected refresh intervals. Snapshot arrival does not refresh an old source sample.

Metric Availability uses available, unsupported, absent, temporarily unavailable, or error. Metric Freshness and adapter enablement are separate: stale values retain their source age, and a disabled adapter is explicitly labeled rather than treated as a measured zero. M2 defines their protocol representation.

## Privacy

Agent adapters inspect same-user process identities and exact resolved executable or bundle paths, never a bare process name, so an unrelated same-named binary is never mistaken for the supported agent; Grok Build's `grok` executable in particular is only recognized under its documented `~/.grok/bin` install path because Homebrew separately distributes an unrelated tool under the same bare name. Adapters do not inspect command-line arguments, which may contain secrets and do not provide trustworthy task state.

macOS gates CoreWLAN SSID/BSSID behind Location Services even though the product does not need coordinates. A foreground setup surface may request this optional permission only after explaining that it enables connected-WiFi identity and stronger Alert correlation; the Operator may skip or deny it without disabling monitoring. No coordinates or movement history are collected or retained.

The daemon supplies Host Station Identity and link state without Location permission when an active WiFi interface is available; absent or unavailable identity remains explicit. Firmware may infer a Host-associated BSS from passively observed frames involving that identity. A granted permission may still yield no SSID/BSSID on some macOS versions, so Association Context Availability distinguishes permission not requested, denied, restricted, authorized but unavailable, unsupported, and error from available context.

## Primary references

- [Mach host statistics](https://developer.apple.com/documentation/kernel/1502863-host_statistics64)
- [VM statistics](https://developer.apple.com/documentation/kernel/vm_statistics64_data_t)
- [APFS capacity semantics](https://developer.apple.com/documentation/foundation/about-apple-file-system)
- [IOBlockStorageDriver statistics](https://developer.apple.com/documentation/kernel/ioblockstoragedriver)
- [64-bit interface data](https://developer.apple.com/documentation/kernel/if_data64)
- [IOPowerSources](https://developer.apple.com/documentation/iokit/iopowersources_h)
- [Metal GPU counter scope](https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers)
- [Launch Agent model](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html)
