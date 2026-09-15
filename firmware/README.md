<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# firmware/

ESP-IDF C++ application for the E32R28T, plus the platform-independent domain
core shared with `simulator/`.

```text
firmware/
├── domain/                pure C++, no ESP-IDF headers; links into ESP32,
│                          simulator, and native tests
├── main/                  ESP-IDF app entrypoint (idf.py build)
├── platform/esp32/        board drivers (display, touch, LED, audio, storage)
├── ui/                    LVGL screens and widgets
└── tools/                 build-time generators (profile TOML -> header)
```

## Ownership

- `domain/`, `main/`, root `CMakeLists.txt`, `sdkconfig.defaults`, and
  `tools/generate_profile_header.py`: F02.
- `platform/esp32/` drivers and `ui/` screens: F05 onward.
- Pins and capabilities live only in `hardware/profiles/*/profile.toml`;
  application code includes the generated header rather than restating pin
  numbers.

## Building

ESP-IDF must be installed and sourced (`source $IDF_PATH/export.sh`); see
[docs/development.md](../docs/development.md).

```sh
./dev build firmware   # idf.py build
./dev check firmware   # native domain test, no ESP-IDF required
./dev run firmware -- monitor
```

`./dev check firmware` builds and runs `firmware/domain/tests` with a plain
host C++ compiler, proving the domain target is usable independently of
ESP-IDF.
