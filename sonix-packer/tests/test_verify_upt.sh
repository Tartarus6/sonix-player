#!/bin/bash
#
# Tests for verify_upt.sh. Builds two tiny .upt files from scratch: one that
# passes every check and one that must fail, and checks the exit codes and the
# message. No stock firmware, no device and no network.
#
# usage: ./tests/test_verify_upt.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERIFY="$HERE/../verify_upt.sh"

for tool in 7z mksquashfs mkisofs split strings; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "skip: missing $tool"
		exit 0
	}
done

TMP="$(mktemp -d "${TMPDIR:-/tmp}/verify-upt-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

md5() { md5sum "$1" | awk '{print $1}'; }
size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }

# Split an image exactly as the packer does: 512 KiB chunks whose names carry
# the previous chunk's md5, and a chain file with one md5 per line.
write_chunks() {
	local name="$1" image="$2" whole current count suffix chunk f
	whole="$(md5 "$image")"
	split -b 524288 -a 4 "$image" "$TMP/chunk_"
	rm -f "$image"
	: >"$TMP/ota_md5_$name.$whole"
	current="$whole"
	count=0
	for f in "$TMP/chunk_"*; do
		[ -e "$f" ] || continue
		suffix="$(printf '%04d' "$count")"
		chunk="$TMP/$name.$suffix.$current"
		mv "$f" "$chunk"
		current="$(md5 "$chunk")"
		echo "$current" >>"$TMP/ota_md5_$name.$whole"
		count=$((count + 1))
	done
}

# make_image MODEL MODULE_VERMAGIC ROOTFS_PADDING_KIB
make_image() {
	local model="$1" vermagic="$2" padding="$3"
	local work="$TMP/$model"
	mkdir -p "$work/root/module_driver" "$work/ota_v0"

	# A file the kernel would be asked to load. Its vermagic is the only thing
	# this test cares about; it is not a real module.
	printf 'fake module\nvermagic=%s preempt mod_unload MIPS32_R2 32BIT \n' "$vermagic" \
		>"$work/root/module_driver/fake.ko"
	# Padding makes the rootfs large enough to exercise the block budget.
	dd if=/dev/zero of="$work/root/padding" bs=1024 count="$padding" 2>/dev/null

	# A fake kernel: strings in the verifier look for a line that starts with
	# Linux-. The uImage header would be the real source; this is enough.
	printf 'uImage\nLinux-4.4.94-sonix\n' >"$work/xImage"

	mksquashfs "$work/root" "$work/rootfs.squashfs" -comp lzo -all-root >/dev/null

	{
		echo "ota_version=0"
		echo
		echo "img_type=kernel"
		echo "img_name=xImage"
		echo "img_size=$(size "$work/xImage")"
		echo "img_md5=$(md5 "$work/xImage")"
		echo
		echo "img_type=rootfs"
		echo "img_name=rootfs.squashfs"
		echo "img_size=$(size "$work/rootfs.squashfs")"
		echo "img_md5=$(md5 "$work/rootfs.squashfs")"
	} >"$work/ota_v0/ota_update.in"
	: >"$work/ota_v0/ota_v0.ok"

	write_chunks xImage "$work/xImage"
	write_chunks rootfs.squashfs "$work/rootfs.squashfs"
	# write_chunks writes into $TMP, not $work; move the result into the ISO tree.
	mv "$TMP"/xImage.* "$TMP"/ota_md5_xImage.* "$TMP"/rootfs.squashfs.* \
		"$TMP"/ota_md5_rootfs.squashfs.* "$work/ota_v0/" 2>/dev/null || true

	mkisofs -o "$TMP/$model.upt" -J -r "$work" >/dev/null 2>&1
}

# 3 MiB partition, 128 KiB blocks: 24 blocks. The tiny rootfs leaves plenty.
cat >"$TMP/image.lock" <<'EOF'
TEST|0x300000|0x20000|0x300000
EOF

make_image TEST 4.4.94-sonix 64
if ! "$VERIFY" --model TEST --upt "$TMP/TEST.upt" --lock "$TMP/image.lock" >"$TMP/ok.log" 2>&1; then
	echo "FAIL: the valid image was rejected"
	cat "$TMP/ok.log"
	exit 1
fi
grep -q '^OK ' "$TMP/ok.log" || {
	echo "FAIL: the valid image did not end with OK"
	cat "$TMP/ok.log"
	exit 1
}

# The same image, but the module was built for another kernel: the verifier
# must reject it even though the size is fine.
make_image BADMOD 4.4.94+ 64
cat >"$TMP/image.lock" <<'EOF'
BADMOD|0x300000|0x20000|0x300000
EOF
if "$VERIFY" --model BADMOD --upt "$TMP/BADMOD.upt" --lock "$TMP/image.lock" >"$TMP/badmod.log" 2>&1; then
	echo "FAIL: a module built for another kernel was accepted"
	cat "$TMP/badmod.log"
	exit 1
fi
grep -q 'built for 4.4.94+, the kernel is 4.4.94-sonix' "$TMP/badmod.log" || {
	echo "FAIL: the mismatch was not named"
	cat "$TMP/badmod.log"
	exit 1
}

# A chunk whose name no longer carries the previous md5 must be refused.
cat >"$TMP/image.lock" <<'EOF'
TEST|0x300000|0x20000|0x300000
EOF
mkdir -p "$TMP/broken"
7z x "$TMP/TEST.upt" -o"$TMP/broken" -y >/dev/null
first="$(echo "$TMP/broken/ota_v0/rootfs.squashfs.0000."*)"
mv "$first" "$TMP/broken/ota_v0/rootfs.squashfs.0000.00000000000000000000000000000000"
mkisofs -o "$TMP/BROKEN.upt" -J -r "$TMP/broken" >/dev/null 2>&1
if "$VERIFY" --model TEST --upt "$TMP/BROKEN.upt" --lock "$TMP/image.lock" >"$TMP/broken.log" 2>&1; then
	echo "FAIL: a broken md5 chain was accepted"
	cat "$TMP/broken.log"
	exit 1
fi
grep -q 'chunk' "$TMP/broken.log" || {
	echo "FAIL: the broken chain was not named"
	cat "$TMP/broken.log"
	exit 1
}

# The same small image in a partition that is almost full: the rootfs must be
# rejected for leaving fewer than eight erase blocks free.
cat >"$TMP/image.lock" <<'EOF'
TEST|0x40000|0x20000|0x40000
EOF
if "$VERIFY" --model TEST --upt "$TMP/TEST.upt" --lock "$TMP/image.lock" >"$TMP/big.log" 2>&1; then
	echo "FAIL: a rootfs with no bad-block room was accepted"
	cat "$TMP/big.log"
	exit 1
fi
grep -q 'erase block' "$TMP/big.log" || {
	echo "FAIL: the rootfs budget was not named"
	cat "$TMP/big.log"
	exit 1
}

echo "verify_upt tests: ok"
