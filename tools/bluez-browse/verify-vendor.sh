#!/bin/bash
# Check that a rebuilt bluetoothd still exposes the vendor-visible strings and
# (when a vendor binary is supplied) the same ELF ABI.
#
#   verify-vendor.sh BUILT [VENDOR]
set -euo pipefail

BUILT=${1:-}
VENDOR=${2:-}

fail=0
check() {
	if "$@"; then
		printf 'ok   %s\n' "$*"
	else
		printf 'FAIL %s\n' "$*" >&2
		fail=$((fail + 1))
	fi
}

have_string() {
	local tmp rc
	tmp=$(mktemp "${TMPDIR:-/tmp}/bb-strings.XXXXXX")
	strings -a "$1" >"$tmp"
	grep -Fq -- "$2" "$tmp"
	rc=$?
	rm -f "$tmp"
	return "$rc"
}

if [ -z "$BUILT" ] || [ ! -f "$BUILT" ]; then
	echo "usage: verify-vendor.sh BUILT [VENDOR]" >&2
	exit 2
fi

for s in \
	'/var/run/sys_client' \
	'BT:CHANGE %s AudioSource %s %d' \
	'BT:CHANGE %s AudioSink %s %d' \
	'BT:CHANGE %s HID %s' \
	'BT:MEDIA %s: %s' \
	'/usr/data/bluetooth' \
	'bluez: vendor bcm set high priority'; do
	check have_string "$BUILT" "$s"
done

if [ -n "$VENDOR" ] && [ -f "$VENDOR" ]; then
	for s in \
		'/var/run/sys_client' \
		'BT:CHANGE %s AudioSource %s %d' \
		'BT:CHANGE %s AudioSink %s %d' \
		'BT:CHANGE %s HID %s' \
		'BT:MEDIA %s: %s' \
		'/usr/data/bluetooth'; do
		check have_string "$VENDOR" "$s"
	done

	if command -v readelf >/dev/null 2>&1; then
		built_machine=$(readelf -h "$BUILT" | awk -F: '/Machine:/ {gsub(/^ +/,"",$2); print $2}')
		vendor_machine=$(readelf -h "$VENDOR" | awk -F: '/Machine:/ {gsub(/^ +/,"",$2); print $2}')
		check test "$built_machine" = "$vendor_machine"
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo "$fail vendor compatibility check(s) failed" >&2
	exit 1
fi

echo 'vendor compatibility: OK'
