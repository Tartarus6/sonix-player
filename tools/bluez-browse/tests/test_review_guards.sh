#!/bin/bash
# Aggregate guard test: all host harnesses, patch application, review markers.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR=${TMPDIR:-/home/diogo/Downloads/Hiby/pr-work/tmp}
BUILD=$TMPDIR/bluez-browse-tests
mkdir -p "$BUILD"

cd "$ROOT/tests"

for t in run.sh run-item-type.sh run-display-name.sh run-change-count.sh \
	 run-parent.sh run-tg-pending.sh run-defer.sh run-dbus-sim.sh; do
	./"$t"
done

python3 ./check-player-features.py

# Every required patch must exist and be numbered in order.
patches=("$ROOT"/patches/*.patch)
[ "${#patches[@]}" -eq 12 ] || { echo "FAIL: expected 12 patches, got ${#patches[@]}" >&2; exit 1; }

# Review markers: the behaviours the patches must contain.
grep -q 'avctp_send_browsing_rsp' "$ROOT/patches/12-avctp.patch"
grep -q '0x68' "$ROOT/patches/15-player-browsable.patch"
grep -q 'depth' "$ROOT/modules/avrcp-tg-browse.c"
grep -q 'MEDIA_BROWSE_UID_CACHE' "$ROOT/modules/media-browse.c"
grep -q 'tg_pending_drop_player' "$ROOT/modules/tg-pending.c"
grep -q 'avctp_defer_save' "$ROOT/modules/avctp-defer.c"
grep -q 'BT:CHANGE %s AudioSource %s %d' "$ROOT/patches/02-notify-socket.patch"
grep -q '/usr/data/bluetooth' "$ROOT/patches/01-storage-root.patch"

echo 'PASS review-guards'
