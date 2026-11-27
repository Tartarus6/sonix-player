#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
APP="$ROOT/sonix-player"
TMPDIR=${TMPDIR:-$ROOT/../tmp}
mkdir -p "$TMPDIR"
OUT="$TMPDIR/fallback_harness"

gcc -std=gnu11 -Wall -Wextra -Werror -I"$APP" \
	"$APP/tests/harness/harness.c" \
	"$APP/tests/harness/fallback.c" \
	-o "$OUT"

"$OUT"
