#!/usr/bin/env python3
"""Check the advertised AVRCP player feature mask.

features[7] must be 0x68: the stock 0x60 plus 0x08 (Browsing).  Fail loudly if
the TG browsing feature bit is ever dropped.
"""
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'patches', '15-player-browsable.patch')
text = open(path, encoding='utf-8').read()

m = re.search(r'\+\s*0xFB,\s*0x3F,\s*0x([0-9A-Fa-f]{2})', text)
if m is None:
    m = re.search(r'0xFB,\s*0x3F,\s*0x([0-9A-Fa-f]{2})', text)
if m is None:
    sys.exit('FAIL: cannot find features[7] value')

value = int(m.group(1), 16)
if value != 0x68:
    sys.exit('FAIL: features[7]=0x%02x, expected 0x68' % value)
if not value & 0x08:
    sys.exit('FAIL: browsing feature bit (0x08) is clear')

print('PASS player-features: features[7]=0x%02x (browsing advertised)' % value)
