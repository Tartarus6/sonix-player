#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR=${TMPDIR:-/home/diogo/Downloads/Hiby/pr-work/tmp}
BUILD="$TMPDIR/bluez-browse-tests"
mkdir -p "$BUILD"

CFLAGS="-std=gnu11 -Wall -Wextra -I$ROOT/modules -I$ROOT/tests"
CFLAGS="$CFLAGS $(pkg-config --cflags glib-2.0)"
LIBS="$(pkg-config --libs glib-2.0)"

cc $CFLAGS -o "$BUILD/test_tg_pending" \
	"$ROOT/tests/test_tg_pending.c" "$ROOT/modules/tg-pending.c" $LIBS
exec "$BUILD/test_tg_pending"
