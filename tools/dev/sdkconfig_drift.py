#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Warn when an existing sdkconfig differs from project defaults."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "firmware"


def read(path):
    values = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'(CONFIG_\w+)=(.*)', line)
        unset = re.fullmatch(r'# (CONFIG_\w+) is not set', line)
        if match:
            values[match[1]] = match[2]
        elif unset:
            values[unset[1]] = 'n'
    return values


def main():
    defaults = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'sdkconfig.defaults'
    config = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'sdkconfig'
    if not config.exists():
        return
    actual = read(config)
    differences = [(k, v, actual.get(k, 'missing')) for k, v in read(defaults).items()
                   if actual.get(k) != v]
    if differences:
        print('Warning: firmware sdkconfig differs from defaults:')
        for key, expected, found in differences:
            print(f'  {key}: default={expected}, sdkconfig={found}')
        print('Use --fresh, or move firmware/sdkconfig aside and rebuild.')


if __name__ == '__main__':
    main()
