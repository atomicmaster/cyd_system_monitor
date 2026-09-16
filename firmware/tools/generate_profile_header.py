#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate a constexpr C++ header from a hardware profile TOML file.

Pins and geometry live in hardware/profiles/<id>/profile.toml. Application
and domain modules include the generated header instead of restating pin
numbers, so a new profile only requires a new TOML file.

This does not use the stdlib `tomllib` module: ESP-IDF v5.3's own managed
Python environment (used to run this script during `idf.py build`, inside
and outside CI) can be as old as Python 3.10, which predates `tomllib`
(3.11+). `parse_flat_toml` below is a dependency-free parser for exactly the
subset of TOML the hardware profiles actually use: flat `[section]` tables
of scalar `key = value` assignments (quoted strings, decimal/hex integers,
booleans). Anything outside that subset fails clearly rather than silently
misparsing.
"""
import argparse
import pathlib
import re
import sys

_SECTION_RE = re.compile(r"^\[([A-Za-z0-9_]+)\]$")
_ASSIGNMENT_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$")


def _parse_value(raw: str, context: str):
    if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
        return raw[1:-1]
    if raw in ("true", "false"):
        return raw == "true"
    if raw.lower().startswith("0x"):
        return int(raw, 16)
    if re.fullmatch(r"-?\d+", raw):
        return int(raw)
    raise ValueError(f"unsupported TOML value {raw!r} in {context}")


def parse_flat_toml(text: str) -> dict:
    """Parses flat `[section]` tables of scalar assignments. See module docstring."""
    profile = {}
    section = None
    section_name = "<top-level>"
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        context = f"line {lineno}"

        section_match = _SECTION_RE.match(line)
        if section_match:
            section_name = section_match.group(1)
            section = profile.setdefault(section_name, {})
            continue

        assignment_match = _ASSIGNMENT_RE.match(line)
        if not assignment_match:
            raise ValueError(f"unparseable TOML line at {context}: {raw_line!r}")
        key, raw_value = assignment_match.groups()
        value = _parse_value(raw_value.strip(), f"{section_name}.{key} ({context})")

        if section is None:
            profile[key] = value
        else:
            section[key] = value
    return profile


TEMPLATE = """// SPDX-License-Identifier: Apache-2.0
// Generated from {source} by firmware/tools/generate_profile_header.py.
// Do not edit by hand.
#pragma once

namespace firmware::domain::profile {{

inline constexpr char kProfileId[] = "{profile_id}";
inline constexpr int kDisplayWidth = {display_width};
inline constexpr int kDisplayHeight = {display_height};
inline constexpr int kLogicalWidth = {logical_width};
inline constexpr int kLogicalHeight = {logical_height};
inline constexpr bool kTouchRequired = {touch_required};

// Display (ILI9341V), 4-wire SPI pins plus backlight PWM pin. miso is
// wired (profile pin 12) even though many ILI9341 wiring diagrams omit a
// read path; the platform driver still configures it since the profile
// defines it.
inline constexpr int kDisplayCs = {display_cs};
inline constexpr int kDisplayDc = {display_dc};
inline constexpr int kDisplaySclk = {display_sclk};
inline constexpr int kDisplayMosi = {display_mosi};
inline constexpr int kDisplayMiso = {display_miso};
inline constexpr int kDisplayBacklight = {display_backlight};

// display.reset and buttons.reset are both the chip's EN pin in profile.toml
// (a string, not a GPIO number): EN is not a GPIO the application drives, so
// it is documented here rather than emitted as a constant.

// Touch (XPT2046), SPI pins and guided-calibration parameters.
inline constexpr int kTouchSclk = {touch_sclk};
inline constexpr int kTouchMosi = {touch_mosi};
inline constexpr int kTouchMiso = {touch_miso};
inline constexpr int kTouchCs = {touch_cs};
inline constexpr int kTouchIrq = {touch_irq};
inline constexpr int kTouchCalibrationTargets = {touch_calibration_targets};
inline constexpr bool kTouchCalibrateOnFirstStart = {touch_calibrate_on_first_start};

// RGB status LED (common anode, active low per profile).
inline constexpr int kRgbLedRed = {rgb_led_red};
inline constexpr int kRgbLedGreen = {rgb_led_green};
inline constexpr int kRgbLedBlue = {rgb_led_blue};
inline constexpr bool kRgbLedActiveLow = {rgb_led_active_low};

// MicroSD, SPI pins. mosi/sclk/miso intentionally match expansion_spi's
// pins below: this profile wires MicroSD and the expansion header onto the
// same physical SPI bus. See hardware/profiles/.../README.md's open
// three-buses-vs-two-controllers question, resolved provisionally by F06
// and validated by F08.
inline constexpr int kMicrosdCs = {microsd_cs};
inline constexpr int kMicrosdMosi = {microsd_mosi};
inline constexpr int kMicrosdSclk = {microsd_sclk};
inline constexpr int kMicrosdMiso = {microsd_miso};

// Expansion SPI shares its mosi/sclk/miso with MicroSD; only its CS differs.
inline constexpr int kExpansionSpiCs = {expansion_spi_cs};

// Audio: enable-pin GPIO plus the built-in DAC channel pin.
inline constexpr int kAudioEnable = {audio_enable};
inline constexpr bool kAudioEnableActiveLow = {audio_enable_active_low};
inline constexpr int kAudioDac = {audio_dac};
inline constexpr bool kAudioAlertSoundDefault = {audio_alert_sound_default};

// Battery voltage ADC input (diagnostic only; no calibrated state of charge).
inline constexpr int kBatteryAdc = {battery_adc};

// BOOT/download button (diagnostic input only). buttons.reset ("EN") is
// documented above alongside display.reset; it is not a GPIO constant.
inline constexpr int kButtonDownload = {button_download};

// UART0 / USB serial bridge pins.
inline constexpr int kUart0Rx = {uart0_rx};
inline constexpr int kUart0Tx = {uart0_tx};

// Input-only expansion GPIO.
inline constexpr int kExpansionInputOnly = {expansion_input_only};

}}  // namespace firmware::domain::profile
"""


def _require(section: dict, keys, section_name: str, profile_path) -> list:
    missing = [key for key in keys if key not in section]
    if missing:
        print(
            f"error: {profile_path} is missing {section_name} fields: {missing}",
            file=sys.stderr,
        )
    return missing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile_toml", type=pathlib.Path)
    parser.add_argument("output_header", type=pathlib.Path)
    args = parser.parse_args()

    profile = parse_flat_toml(args.profile_toml.read_text())

    display = profile.get("display", {})
    touch = profile.get("touch", {})
    rgb_led = profile.get("rgb_led", {})
    microsd = profile.get("microsd", {})
    expansion_spi = profile.get("expansion_spi", {})
    audio = profile.get("audio", {})
    battery = profile.get("battery", {})
    buttons = profile.get("buttons", {})
    uart0 = profile.get("uart0", {})
    expansion = profile.get("expansion", {})

    missing = []
    missing += _require(
        display,
        ("width", "height", "logical_width", "logical_height", "cs", "dc", "sclk", "mosi", "miso",
         "backlight"),
        "display",
        args.profile_toml,
    )
    missing += _require(touch, ("sclk", "mosi", "miso", "cs", "irq", "calibration_targets"), "touch", args.profile_toml)
    missing += _require(rgb_led, ("red", "green", "blue", "active_low"), "rgb_led", args.profile_toml)
    missing += _require(microsd, ("cs", "mosi", "sclk", "miso"), "microsd", args.profile_toml)
    missing += _require(expansion_spi, ("cs",), "expansion_spi", args.profile_toml)
    missing += _require(
        audio, ("enable", "enable_active_low", "dac", "alert_sound_default"), "audio", args.profile_toml
    )
    missing += _require(battery, ("adc",), "battery", args.profile_toml)
    missing += _require(buttons, ("download",), "buttons", args.profile_toml)
    missing += _require(uart0, ("rx", "tx"), "uart0", args.profile_toml)
    missing += _require(expansion, ("input_only",), "expansion", args.profile_toml)
    if missing:
        return 1

    def cbool(value) -> str:
        return "true" if value else "false"

    rendered = TEMPLATE.format(
        source=args.profile_toml.as_posix(),
        profile_id=profile.get("profile_id", "unknown"),
        display_width=display["width"],
        display_height=display["height"],
        logical_width=display["logical_width"],
        logical_height=display["logical_height"],
        touch_required="true" if touch.get("required_for_active_variant") else "false",
        display_cs=display["cs"],
        display_dc=display["dc"],
        display_sclk=display["sclk"],
        display_mosi=display["mosi"],
        display_miso=display["miso"],
        display_backlight=display["backlight"],
        touch_sclk=touch["sclk"],
        touch_mosi=touch["mosi"],
        touch_miso=touch["miso"],
        touch_cs=touch["cs"],
        touch_irq=touch["irq"],
        touch_calibration_targets=touch["calibration_targets"],
        touch_calibrate_on_first_start=cbool(touch.get("calibrate_on_first_start")),
        rgb_led_red=rgb_led["red"],
        rgb_led_green=rgb_led["green"],
        rgb_led_blue=rgb_led["blue"],
        rgb_led_active_low=cbool(rgb_led["active_low"]),
        microsd_cs=microsd["cs"],
        microsd_mosi=microsd["mosi"],
        microsd_sclk=microsd["sclk"],
        microsd_miso=microsd["miso"],
        expansion_spi_cs=expansion_spi["cs"],
        audio_enable=audio["enable"],
        audio_enable_active_low=cbool(audio["enable_active_low"]),
        audio_dac=audio["dac"],
        audio_alert_sound_default=cbool(audio["alert_sound_default"]),
        battery_adc=battery["adc"],
        button_download=buttons["download"],
        uart0_rx=uart0["rx"],
        uart0_tx=uart0["tx"],
        expansion_input_only=expansion["input_only"],
    )

    args.output_header.parent.mkdir(parents=True, exist_ok=True)
    args.output_header.write_text(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
