#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR=${TMPDIR:-/home/diogo/Downloads/Hiby/pr-work/tmp}
BUILD="$TMPDIR/bluez-browse-tests"
mkdir -p "$BUILD"

CFLAGS="-std=gnu11 -Wall -Wextra -I$ROOT/modules -I$ROOT/tests"
CFLAGS="$CFLAGS $(pkg-config --cflags glib-2.0 dbus-1)"
LIBS="$(pkg-config --libs glib-2.0 dbus-1)"

cc $CFLAGS -o "$BUILD/test_parent" \
	"$ROOT/tests/test_parent.c" "$ROOT/modules/media-browse.c" \
	"$ROOT/modules/avrcp-tg-browse.c" $LIBS
exec "$BUILD/test_parent"
