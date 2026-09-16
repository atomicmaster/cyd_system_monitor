#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unit tests for generate_profile_header.py's dependency-free TOML subset
parser. Run directly: python3 firmware/tools/test_generate_profile_header.py
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from generate_profile_header import parse_flat_toml  # noqa: E402


class ParseFlatTomlTests(unittest.TestCase):
    def test_parses_scalar_types(self):
        profile = parse_flat_toml(
            "\n".join(
                [
                    "[section]",
                    'name = "hello world"',
                    "enabled = true",
                    "disabled = false",
                    "count = 42",
                    "negative = -5",
                    "vendor_id = 0x1a86",
                ]
            )
        )
        self.assertEqual(
            profile["section"],
            {
                "name": "hello world",
                "enabled": True,
                "disabled": False,
                "count": 42,
                "negative": -5,
                "vendor_id": 0x1A86,
            },
        )

    def test_ignores_blank_lines_and_comments(self):
        profile = parse_flat_toml("\n".join(["# a comment", "", "[section]", "", "x = 1"]))
        self.assertEqual(profile["section"]["x"], 1)

    def test_top_level_assignment_before_any_section(self):
        profile = parse_flat_toml('profile_id = "abc"\n[section]\nx = 1')
        self.assertEqual(profile["profile_id"], "abc")
        self.assertEqual(profile["section"]["x"], 1)

    def test_rejects_unsupported_value(self):
        with self.assertRaises(ValueError):
            parse_flat_toml("[section]\nx = [1, 2]")

    def test_rejects_unparseable_line(self):
        with self.assertRaises(ValueError):
            parse_flat_toml("[section]\nnot an assignment")

    def test_real_profile_parses(self):
        profile_path = (
            pathlib.Path(__file__).parent
            / ".."
            / ".."
            / "hardware/profiles/lcdwiki-esp32-32e-2.8/profile.toml"
        )
        profile = parse_flat_toml(profile_path.read_text())
        self.assertEqual(profile["display"]["logical_width"], 320)
        self.assertEqual(profile["display"]["logical_height"], 240)
        self.assertTrue(profile["touch"]["required_for_active_variant"])
        self.assertEqual(profile["display"]["cs"], 15)
        self.assertEqual(profile["display"]["dc"], 2)
        self.assertEqual(profile["display"]["sclk"], 14)
        self.assertEqual(profile["display"]["mosi"], 13)
        self.assertEqual(profile["display"]["miso"], 12)
        self.assertEqual(profile["display"]["backlight"], 21)
        # Fields F06/F07 surface as generated constants.
        self.assertEqual(profile["touch"]["sclk"], 25)
        self.assertEqual(profile["touch"]["mosi"], 32)
        self.assertEqual(profile["touch"]["miso"], 39)
        self.assertEqual(profile["touch"]["cs"], 33)
        self.assertEqual(profile["touch"]["irq"], 36)
        self.assertEqual(profile["touch"]["calibration_targets"], 5)
        self.assertTrue(profile["touch"]["calibrate_on_first_start"])
        self.assertEqual(profile["rgb_led"]["red"], 22)
        self.assertEqual(profile["rgb_led"]["green"], 16)
        self.assertEqual(profile["rgb_led"]["blue"], 17)
        self.assertTrue(profile["rgb_led"]["active_low"])
        self.assertEqual(profile["microsd"]["cs"], 5)
        self.assertEqual(profile["microsd"]["mosi"], 23)
        self.assertEqual(profile["microsd"]["sclk"], 18)
        self.assertEqual(profile["microsd"]["miso"], 19)
        self.assertEqual(profile["expansion_spi"]["cs"], 27)
        self.assertEqual(profile["audio"]["enable"], 4)
        self.assertTrue(profile["audio"]["enable_active_low"])
        self.assertEqual(profile["audio"]["dac"], 26)
        self.assertFalse(profile["audio"]["alert_sound_default"])
        self.assertEqual(profile["battery"]["adc"], 34)
        self.assertEqual(profile["buttons"]["download"], 0)
        self.assertEqual(profile["buttons"]["reset"], "EN")
        self.assertEqual(profile["display"]["reset"], "EN")
        self.assertEqual(profile["uart0"]["rx"], 3)
        self.assertEqual(profile["uart0"]["tx"], 1)
        self.assertEqual(profile["expansion"]["input_only"], 35)

    def test_generated_header_declares_new_constants(self):
        import tempfile

        from generate_profile_header import main as generate_main

        profile_path = (
            pathlib.Path(__file__).parent
            / ".."
            / ".."
            / "hardware/profiles/lcdwiki-esp32-32e-2.8/profile.toml"
        )
        with tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / "profile.hpp"
            argv = sys.argv
            sys.argv = ["generate_profile_header.py", str(profile_path), str(output)]
            try:
                rc = generate_main()
            finally:
                sys.argv = argv
            self.assertEqual(rc, 0)
            text = output.read_text()
            for symbol in (
                "kDisplayCs",
                "kDisplayDc",
                "kDisplaySclk",
                "kDisplayMosi",
                "kDisplayMiso",
                "kDisplayBacklight",
                "kTouchSclk",
                "kTouchMosi",
                "kTouchMiso",
                "kTouchCs",
                "kTouchIrq",
                "kTouchCalibrationTargets",
                "kTouchCalibrateOnFirstStart",
                "kRgbLedRed",
                "kRgbLedGreen",
                "kRgbLedBlue",
                "kRgbLedActiveLow",
                "kMicrosdCs",
                "kMicrosdMosi",
                "kMicrosdSclk",
                "kMicrosdMiso",
                "kExpansionSpiCs",
                "kAudioEnable",
                "kAudioEnableActiveLow",
                "kAudioDac",
                "kAudioAlertSoundDefault",
                "kBatteryAdc",
                "kButtonDownload",
                "kUart0Rx",
                "kUart0Tx",
                "kExpansionInputOnly",
            ):
                self.assertIn(symbol, text, f"{symbol} missing from generated header")
            self.assertIn('kAudioAlertSoundDefault = false', text)
            self.assertIn('kTouchCalibrateOnFirstStart = true', text)


if __name__ == "__main__":
    unittest.main()
