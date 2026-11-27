#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
APP="$ROOT/sonix-player"
TMPDIR=${TMPDIR:-$ROOT/../tmp}
mkdir -p "$TMPDIR"
OUT="$TMPDIR/btbrowser_test"

gcc -std=gnu11 -Wall -Wextra -Werror -pthread -I"$APP" \
	"$APP/tests/btbrowser_test.c" \
	"$APP/tests/fake_provider.c" \
	"$APP/tests/support/dbuslite_stub.c" \
	"$APP/src/system/bluetooth/btbrowser.c" \
	-o "$OUT"

"$OUT"
