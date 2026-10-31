# SPDX-License-Identifier: GPL-3.0-only
import subprocess
import sys
import tempfile
from pathlib import Path

script = Path(__file__).resolve().parents[1] / 'sdkconfig_drift.py'
with tempfile.TemporaryDirectory() as tmp:
    defaults = Path(tmp) / 'defaults'
    config = Path(tmp) / 'config'
    defaults.write_text('CONFIG_A=n\nCONFIG_B=y\nCONFIG_C=3\n')
    config.write_text('# CONFIG_A is not set\nCONFIG_B=n\n')
    output = subprocess.check_output([sys.executable, str(script), str(defaults), str(config)], text=True)
    assert 'CONFIG_A' not in output and 'CONFIG_B' in output and 'CONFIG_C' in output
    assert 'missing' in output and '--fresh' in output
print('sdkconfig drift test passed')
