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

}}  // namespace firmware::domain::profile
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile_toml", type=pathlib.Path)
    parser.add_argument("output_header", type=pathlib.Path)
    args = parser.parse_args()

    profile = parse_flat_toml(args.profile_toml.read_text())

    display = profile.get("display", {})
    touch = profile.get("touch", {})

    missing = [
        key
        for key in ("width", "height", "logical_width", "logical_height")
        if key not in display
    ]
    if missing:
        print(
            f"error: {args.profile_toml} is missing display fields: {missing}",
            file=sys.stderr,
        )
        return 1

    rendered = TEMPLATE.format(
        source=args.profile_toml.as_posix(),
        profile_id=profile.get("profile_id", "unknown"),
        display_width=display["width"],
        display_height=display["height"],
        logical_width=display["logical_width"],
        logical_height=display["logical_height"],
        touch_required="true" if touch.get("required_for_active_variant") else "false",
    )

    args.output_header.parent.mkdir(parents=True, exist_ok=True)
    args.output_header.write_text(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
