<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# host/

Rust workspace for the macOS per-user LaunchAgent and CLI.

```text
host/
├── daemon/          cyd-hostd: LaunchAgent skeleton, no external contact yet
├── cli/             cyd-host: pairing/diagnostics/configuration commands
├── metrics/         portable Host Metric seam (MetricAvailability, MetricSample)
├── platform-macos/  macOS collector/permission/service-management adapter home
├── transport/       USB discovery, pairing, and session transport (TB02)
└── protocol/        Host-side codec home, wrapping protocol/generated/rust
```

Concrete collectors, USB transport, and protocol codecs are added by tracer
tickets (TB02 onward); this workspace only fixes the crate boundaries they
land in.

```sh
./dev build host
./dev check host   # fmt --check, clippy -D warnings, test
./dev run host -- --help
```
