#!/bin/sh
# Checks the debug-tool removal block of sonix_firmware_packer.sh.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PACKER="$SCRIPT_DIR/sonix_firmware_packer.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

DEBUG_PATHS="
usr/bin/strace
usr/bin/strace-log-merge
usr/bin/iperf3
usr/lib/libiperf.so.0.0.0
"

AIC_PATHS="
lib/firmware/aic8800d40_sdio_fw
module_driver/aic8800_fdrv.ko
module_driver/aic8800_bsp.ko
module_driver/aic8800_btlpm.ko
module_driver/aic8800_fdrv.load
module_driver/aic8800_bsp.r1
"

failures=0
fail() {
	echo "FAIL: $*" >&2
	failures=$((failures + 1))
}

# The marked block is the single source of truth: it holds STOCK_UI_DIRS,
# STOCK_DEBUG_FILES and strip_debug_tools(), and the test runs exactly it.
BLOCK="$(sed -n '/^# DEBUG-TOOLS-BEGIN$/,/^# DEBUG-TOOLS-END$/p' "$PACKER")"
[ -n "$BLOCK" ] || { echo "FAIL: DEBUG-TOOLS markers not found in $PACKER" >&2; exit 1; }

# The packer must contain its own strip_debug_tools call.
grep -q 'strip_debug_tools "\$SQUASH_DIR"' "$PACKER" ||
	fail "packer has no production strip_debug_tools call"

# Run the extracted block in its own shell. say() is stubbed because the test
# is not the packer. A file avoids quoting the block, which holds $variables.
{
	echo 'say() { :; }'
	printf '%s\n' "$BLOCK"
	echo 'strip_debug_tools "$SQUASH_DIR"'
} > "$WORK/run.sh"

# The same block, but printing STOCK_UI_DIRS for the AIC8800 guard.
{
	printf '%s\n' "$BLOCK"
	echo 'for d in "${STOCK_UI_DIRS[@]}"; do echo "$d"; done'
} > "$WORK/ui.sh"

TREE="$WORK/tree"

make_tree() {
	rm -rf "$TREE"
	mkdir -p "$TREE/usr/bin" "$TREE/usr/lib" "$TREE/lib/firmware" "$TREE/module_driver"
	for f in $DEBUG_PATHS $AIC_PATHS; do
		[ -n "$f" ] || continue
		mkdir -p "$TREE/$(dirname "$f")"
		: > "$TREE/$f"
	done
}

expect_gone() { [ ! -e "$TREE/$1" ] || fail "$1 should be removed"; }
expect_kept() { [ -e "$TREE/$1" ] || fail "$1 should still exist"; }

# Default run: debug tools go, AIC8800 files stay.
make_tree
SQUASH_DIR="$TREE" bash "$WORK/run.sh"
for f in $DEBUG_PATHS; do expect_gone "$f"; done
for f in $AIC_PATHS; do expect_kept "$f"; done

# DEBUG_TOOLS=keep: nothing is removed.
make_tree
DEBUG_TOOLS=keep SQUASH_DIR="$TREE" bash "$WORK/run.sh"
for f in $DEBUG_PATHS; do expect_kept "$f"; done
for f in $AIC_PATHS; do expect_kept "$f"; done

# The stock UI removal list must never name an AIC8800 path.
UI_PATHS="$(bash "$WORK/ui.sh")"
[ -n "$UI_PATHS" ] || fail "STOCK_UI_DIRS is empty in the marked block"
case "$UI_PATHS" in
	*aic8800*) fail "STOCK_UI_DIRS contains an aic8800 path: $UI_PATHS" ;;
esac

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)" >&2
	exit 1
fi
echo "PASS: debug tools stripped by default, kept with DEBUG_TOOLS=keep, AIC8800 untouched"
