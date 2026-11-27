#!/bin/sh
# Run the media-browse harness against a private session bus.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR=${TMPDIR:-/home/diogo/Downloads/Hiby/pr-work/tmp}
BUILD="$TMPDIR/bluez-browse-tests"
mkdir -p "$BUILD"

CFLAGS="-std=gnu11 -Wall -Wextra -pthread -I$ROOT/modules -I$ROOT/tests"
CFLAGS="$CFLAGS $(pkg-config --cflags glib-2.0 dbus-1)"
LIBS="$(pkg-config --libs glib-2.0 dbus-1) -pthread"

cc $CFLAGS -o "$BUILD/test_media_browse" \
	"$ROOT/tests/test_media_browse.c" "$ROOT/modules/media-browse.c" \
	"$ROOT/modules/avrcp-tg-browse.c" $LIBS

if command -v dbus-run-session >/dev/null 2>&1; then
	exec dbus-run-session -- "$BUILD/test_media_browse"
fi

echo "dbus-run-session not found" >&2
exit 1
