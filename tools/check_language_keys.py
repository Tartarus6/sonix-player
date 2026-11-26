#!/usr/bin/env python3
"""Check that shared UI strings exist in every device asset language file.

tr() in src/system/core/lang.c returns the bare tag when a key is missing, so a
key added to one device tree but not the others shows up as raw text in the UI.
This checks the keys introduced for the three-device branch in all .ini files
of all three asset trees. Exit 0 when complete, 1 when any key is missing.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TREES = (
    ROOT / "sonix-packer/assets/R1/usr/resource/sonix/language",
    ROOT / "sonix-packer/assets/R3PII/usr/resource/sonix/language",
    ROOT / "sonix-packer/v1/assets/V1/usr/resource/sonix/language",
)

# Keys added by this branch for screensaver date, stop-after-track and the
# Bluetooth pairing / car-mode rows. All three binaries share this UI code.
KEYS = (
    "screensaver_show_date",
    "musicsettings_stop_after_track",
    "musicsettings_stop_after_track_note",
    "bt_pairing_request",
    "bt_enter_pin",
    "bt_enter_passkey",
    "bt_confirm_passkey",
    "bt_confirm_passkey_hint",
    "bt_display_passkey_hint",
    "bt_car_mode",
    "bt_pairing_consent",
    "bt_pairing_consent_note",
    "bt_pairing_consent_confirm",
    "bt_car_mode_switching",
    "bt_pairing_authorize",
)

KEY_RE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=")


def main() -> int:
    failures = []
    checked = 0
    for tree in TREES:
        if not tree.is_dir():
            failures.append(f"missing language directory: {tree}")
            continue
        for path in sorted(tree.glob("*.ini")):
            checked += 1
            present = set()
            for line in path.read_text(encoding="utf-8").splitlines():
                match = KEY_RE.match(line)
                if match:
                    present.add(match.group(1))
            missing = [key for key in KEYS if key not in present]
            if missing:
                failures.append(f"{path.relative_to(ROOT)}: {', '.join(missing)}")

    if failures:
        print("Language key parity FAILED:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print(f"Language key parity OK: {checked} files, {len(KEYS)} shared keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
