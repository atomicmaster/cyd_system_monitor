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


if __name__ == "__main__":
    unittest.main()
