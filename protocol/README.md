<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# protocol/

Language-neutral schema and generated types shared by firmware and Host.
Consumers never hand-edit generated output.

```text
protocol/
├── schema/            source of truth (TOML today; C01 may extend the format)
├── tools/generate.py  schema -> C++/Rust generator, with a --check drift mode
├── generated/cpp/     generated C++ header
├── generated/rust/    generated Rust crate (protocol-generated)
└── tests/cpp/         C++ consumer smoke test
```

`schema/smoke.toml` remains the M0 generation-tooling smoke fixture.
`schema/core.toml`, [CORE_PROTOCOL.md](CORE_PROTOCOL.md), and
`fixtures/` define the M2 bounded framed-CBOR CPU-snapshot foundation.
The portable C++ codec is linked into `firmware/domain`; the independent Rust
codec lives in `host/protocol`. Future schema fields must preserve the stated
optional-field and frame-size rules.

Licensing follows [CONTRIBUTING.md](../CONTRIBUTING.md): only `generated/cpp/`
and `tests/cpp/` are Apache-2.0, since that is the protocol code actually
compiled into the firmware image. The schema, the generator tool, and
`generated/rust/` feed the GPL-3.0-only Host lane and are licensed
accordingly.

```sh
./dev build protocol   # regenerate generated/cpp and generated/rust
./dev check protocol   # fail on drift, then build+run both consumers
./dev run protocol -- decode protocol/fixtures/cpu-snapshot-v1.hex
```
