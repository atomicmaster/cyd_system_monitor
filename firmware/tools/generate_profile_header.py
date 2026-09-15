#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate a constexpr C++ header from a hardware profile TOML file.

Pins and geometry live in hardware/profiles/<id>/profile.toml. Application
and domain modules include the generated header instead of restating pin
numbers, so a new profile only requires a new TOML file.
"""
import argparse
import pathlib
import sys
import tomllib

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

    with args.profile_toml.open("rb") as f:
        profile = tomllib.load(f)

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
