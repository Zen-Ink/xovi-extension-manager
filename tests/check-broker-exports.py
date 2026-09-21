#!/usr/bin/env python3
"""Check the dispatcher's required ABI tag on every manager broker export."""
from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / 'xovi-extension-manager.xovi').read_text()
checked = 0
for name, metadata in re.findall(r'^export (\S+)\nwith\n(.*?)^end$', source, re.M | re.S):
    if 'xovi-message-broker$simpleSignal' not in metadata:
        continue
    assert re.search(r'^\s*xovi-message-broker\$version\s*=\s*1\s*$', metadata, re.M), name
    checked += 1
assert checked > 0
print(f'PASS broker ABI tags: {checked} exports')
