<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# host/

Rust workspace for the macOS per-user LaunchAgent and CLI.

```text
host/
├── daemon/          cyd-hostd: per-user daemon (owns the serial port; TB08)
├── hostcore/        shared: session runner, identity, lock, control IPC, supervisor, log
├── cli/             cyd-host: run, unpair, reset-calibration, forget, status, pair, stop, service
├── metrics/         portable Host Metric seam (MetricAvailability, MetricSample)
├── platform-macos/  macOS collector/permission/service-management adapter home
├── transport/       HostClient handshake/pairing state machine and serial port (TB02)
└── protocol/        Host-side codec home, wrapping protocol/generated/rust
```

The daemon and CLI share `hostcore`; the local status/control protocol is in
[docs/host-control-ipc.md](../docs/host-control-ipc.md). `cyd-host service
install|uninstall|start|stop|restart|status [--dry-run]` manages the
LaunchAgent (never touched by tests: they use a fake `launchctl` and temp
directories).

Concrete collectors, USB transport, and protocol codecs are added by tracer
tickets (TB02 onward); this workspace only fixes the crate boundaries they
land in.

```sh
./dev build host
./dev check host   # fmt --check, clippy -D warnings, test
./dev run host -- --help
```
