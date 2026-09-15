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

`schema/smoke.toml` is the M0 generation-tooling smoke fixture, not the
product snapshot contract. C01 defines the first real USB message,
golden byte vectors, and malformed-input cases on top of this tooling.

Licensing follows [CONTRIBUTING.md](../CONTRIBUTING.md): only `generated/cpp/`
and `tests/cpp/` are Apache-2.0, since that is the protocol code actually
compiled into the firmware image. The schema, the generator tool, and
`generated/rust/` feed the GPL-3.0-only Host lane and are licensed
accordingly.

```sh
./dev build protocol   # regenerate generated/cpp and generated/rust
./dev check protocol   # fail on drift, then build+run both consumers
```
