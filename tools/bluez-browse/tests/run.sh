#!/bin/sh
# Byte-level AVRCP TG browsing encoder checks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR=${TMPDIR:-/home/diogo/Downloads/Hiby/pr-work/tmp}
BUILD="$TMPDIR/bluez-browse-tests"
mkdir -p "$BUILD"

CFLAGS="-std=gnu11 -Wall -Wextra -I$ROOT/modules -I$ROOT/tests"
CFLAGS="$CFLAGS $(pkg-config --cflags glib-2.0)"
LIBS="$(pkg-config --libs glib-2.0)"

cc $CFLAGS -o "$BUILD/test_avrcp" \
	"$ROOT/tests/test_avrcp.c" "$ROOT/modules/avrcp-tg-browse.c" $LIBS
exec "$BUILD/test_avrcp"
