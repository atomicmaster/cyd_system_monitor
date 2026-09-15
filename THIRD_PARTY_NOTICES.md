<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Third-party notices

Project dependencies retain their upstream copyright notices and license terms. Release builds must generate or update a dependency-license inventory for ESP-IDF components, LVGL, Rust crates, Swift packages, fonts, icons, and any other incorporated material. The project license does not replace those terms.

The firmware image links against Espressif's object-code-only WiFi, Bluetooth-controller, and PHY radio-stack libraries: `esp32-wifi-lib`, `esp32-bt-lib`, and `esp-phy-lib`, each Apache-2.0 licensed with no published source. This is why the firmware image, and any `protocol/` code compiled into it, is itself licensed `Apache-2.0` rather than `GPL-3.0-only`; see [ADR 0027](docs/adr/0027-license-firmware-under-apache-2-0.md) and the supporting research at [docs/research/gpl-esp-idf-licensing.md](docs/research/gpl-esp-idf-licensing.md). The release inventory must record the exact upstream repository and commit/tag pinned for each of these three libraries alongside the other ESP-IDF-bundled components whose source is available (FreeRTOS, LWIP, Mbed TLS, wpa_supplicant, and others).

The local hardware vendor cache under `hardware/profiles/lcdwiki-esp32-32e-2.8/vendor/` contains LCDWiki-linked manuals, datasheets, photos, archives, design files, and a page snapshot. No explicit redistribution license was found for that material. Downloaded files are ignored by Git, excluded from project source and binary releases, and preserved locally with source URLs and SHA-256 checksums. Their presence does not grant a license to redistribute or copy example code.

This notice will be expanded with exact dependency names, versions, copyright holders, license identifiers, and source locations as implementation dependencies are selected.

## M0 toolchain pins

- **Rust 1.97.1** (pinned in [`rust-toolchain.toml`](rust-toolchain.toml)), Apache-2.0/MIT dual-licensed. `host/` and `protocol/generated/rust` have no external crate dependencies yet; concrete collectors and codecs will add crates with their own notices as they land.
- **ESP-IDF v5.3** (target pin for `firmware/`; see [docs/development.md](docs/development.md)). Its bundled libraries and their licenses are covered above and in the release-time dependency inventory this file defers to.
- **LVGL**: not yet pinned. `firmware/ui/` is empty until F05; no build links LVGL at M0.
- **CMake (>= 3.20) and Ninja**: build-time only, not linked into any shipped artifact.
- **Swift 6.4 / Xcode 16+** for `macos/setup-app/`, which has no external Swift package dependencies yet.
