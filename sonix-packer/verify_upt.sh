#!/bin/bash
#
# Verify a Sonix Player .upt before it is published.
#
# The packer calls this after writing the image, and it can be run by hand on
# any image (including the official release and a test image) with the same
# result. It checks the things that turn a bad image into a brick:
#
#   - the rootfs and kernel fit their partitions with room for NAND bad blocks;
#   - the chunks, their sizes and the md5 chains match ota_update.in;
#   - every module in the rootfs was built for the kernel in the image;
#   - the player is the binary that was packed, and the device name is right.
#
# It does not replace a hardware test. It only makes sure the image is worth
# flashing: a mismatch here cannot boot, and a rootfs with no spare erase
# blocks can erase itself on the first bad block.
#
# usage:
#   verify_upt.sh --model R1 --upt r1.upt [--player sonix_player]
#                 [--device "HiBy R1"] [--lock image.lock]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

MODEL=""
UPT=""
PLAYER=""
DEVICE=""
LOCK="$SCRIPT_DIR/image.lock"
ROOTFS_RESERVE_PEB=8
KERNEL_RESERVE_PEB=4
ROOTFS_WARN_PEB=16

say() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() {
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

# Collect the failures that do not stop the rest of the inspection, so one run
# reports all of them instead of the first.
ERRORS=0
err() {
	printf 'FAIL: %s\n' "$*" >&2
	ERRORS=$((ERRORS + 1))
}

while [ $# -gt 0 ]; do
	case "$1" in
	--model)
		MODEL="${2:-}"
		shift 2
		;;
	--upt)
		UPT="${2:-}"
		shift 2
		;;
	--player)
		PLAYER="${2:-}"
		shift 2
		;;
	--device)
		DEVICE="${2:-}"
		shift 2
		;;
	--lock)
		LOCK="${2:-}"
		shift 2
		;;
	-h | --help)
		sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		die "unknown argument: $1"
		;;
	esac
done

[ -n "$MODEL" ] || die "--model is required (R1 or R3PII)"
[ -n "$UPT" ] || die "--upt is required"
[ -f "$UPT" ] || die "the image does not exist: $UPT"
[ -f "$LOCK" ] || die "the model lock does not exist: $LOCK"

for tool in 7z unsquashfs strings awk grep sed sort file; do
	command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
done
if command -v md5sum >/dev/null 2>&1; then
	md5() { md5sum "$1" | awk '{print $1}'; }
else
	command -v md5 >/dev/null 2>&1 || die "missing tool: md5sum or md5"
	md5() { md5 -q "$1"; }
fi
if stat -c%s . >/dev/null 2>&1; then
	size() { stat -c%s "$1"; }
else
	size() { stat -f%z "$1"; }
fi

# model|rootfs partition|erase block|kernel partition
lock_field() {
	awk -F'|' -v model="$MODEL" -v field="$1" '
		$1 == model && NF >= field { print $field; exit }
	' "$LOCK"
}

ROOTFS_PART="$(lock_field 2)"
ERASE_BLOCK="$(lock_field 3)"
KERNEL_PART="$(lock_field 4)"
[ -n "$ROOTFS_PART" ] && [ -n "$ERASE_BLOCK" ] && [ -n "$KERNEL_PART" ] ||
	die "$MODEL is not in $LOCK"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/verify-upt.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

say "verify_upt: $MODEL, $UPT ($(size "$UPT") bytes)"

7z x "$UPT" -o"$TMP/upt" -y >/dev/null || die "cannot unpack $UPT (not an ISO/UPT?)"
OTA="$TMP/upt/ota_v0"
[ -d "$OTA" ] || die "no ota_v0 folder inside the image"
[ -f "$OTA/ota_v0.ok" ] || err "ota_v0.ok is missing"
[ -f "$OTA/ota_update.in" ] || die "ota_update.in is missing"

# Read a field from the kernel or rootfs block of ota_update.in.
ota_field() {
	local type="$1" key="$2" current=""
	while IFS= read -r line; do
		line="${line%$'\r'}"
		case "$line" in
		img_type=*) current="${line#img_type=}" ;;
		"$key"=*)
			if [ "$current" = "$type" ]; then
				printf '%s\n' "${line#*=}"
				return 0
			fi
			;;
		esac
	done <"$OTA/ota_update.in"
	return 1
}

K_TYPE="kernel"
K_NAME="$(ota_field kernel img_name || true)"
K_SIZE="$(ota_field kernel img_size || true)"
K_MD5="$(ota_field kernel img_md5 || true)"
R_TYPE="rootfs"
R_NAME="$(ota_field rootfs img_name || true)"
R_SIZE="$(ota_field rootfs img_size || true)"
R_MD5="$(ota_field rootfs img_md5 || true)"

[ "$K_TYPE" = kernel ] && [ "$K_NAME" = xImage ] || err "ota_update.in has no kernel block"
[ "$R_TYPE" = rootfs ] && [ "$R_NAME" = rootfs.squashfs ] || err "ota_update.in has no rootfs block"
[ -n "$K_SIZE" ] && [ -n "$K_MD5" ] || err "ota_update.in has an incomplete kernel block"
[ -n "$R_SIZE" ] && [ -n "$R_MD5" ] || err "ota_update.in has an incomplete rootfs block"
[ "$ERRORS" -eq 0 ] || die "ota_update.in is not usable; not inspecting the payload"

# The chunk names carry the previous chunk's md5: the first is named after the
# whole image, and every later one after the chunk before it. A wrong chain is
# how a truncated or reordered set of chunks gets written as a valid image.
join_chunks() {
	local name="$1" dest="$2" f
	: >"$dest"
	for f in "$OTA/$name".[0-9][0-9][0-9][0-9].*; do
		[ -e "$f" ] || continue
		cat "$f" >>"$dest"
	done
	[ -s "$dest" ]
}

check_chunks() {
	local name="$1" whole="$2" expected="$3"
	local md5_file="" line="" prev="" sum="" suffix="" f="" i=0 count=0 last=""
	md5_file="$(ls "$OTA/ota_md5_$name."* 2>/dev/null | head -1 || true)"
	[ -n "$md5_file" ] || {
		err "$name: no md5 chain file"
		return
	}
	[ "${md5_file##*.}" = "$whole" ] || err "$name: the md5 chain file is not named after the whole image"

	for f in "$OTA/$name".[0-9][0-9][0-9][0-9].*; do
		[ -e "$f" ] || continue
		sum="$(md5 "$f")"
		line="$(sed -n "$((i + 1))p" "$md5_file")"
		[ "$sum" = "$line" ] || err "$name: chunk $i md5 does not match the chain"
		suffix="${f##*.}"
		if [ "$i" -eq 0 ]; then
			[ "$suffix" = "$whole" ] || err "$name: the first chunk is not named after the whole image"
		else
			[ "$suffix" = "$prev" ] || err "$name: chunk $i is not chained to chunk $((i - 1))"
		fi
		last="$f"
		prev="$sum"
		i=$((i + 1))
	done

	count=$(((expected + 524288 - 1) / 524288))
	[ "$i" -gt 0 ] || {
		err "$name: no chunks"
		return
	}
	[ "$i" -eq "$count" ] || err "$name: $i chunks, expected $count"
	[ "$(size "$last")" -gt 0 ] || err "$name: the last chunk is empty"
}

JOINED_ROOTFS="$TMP/rootfs.squashfs"
JOINED_KERNEL="$TMP/xImage"

if join_chunks rootfs.squashfs "$JOINED_ROOTFS"; then
	[ "$(size "$JOINED_ROOTFS")" = "$R_SIZE" ] || err "rootfs: joined size $(size "$JOINED_ROOTFS"), ota_update.in says $R_SIZE"
	[ "$(md5 "$JOINED_ROOTFS")" = "$R_MD5" ] || err "rootfs: joined md5 does not match ota_update.in"
else
	err "rootfs: no chunks"
fi

if join_chunks xImage "$JOINED_KERNEL"; then
	[ "$(size "$JOINED_KERNEL")" = "$K_SIZE" ] || err "kernel: joined size $(size "$JOINED_KERNEL"), ota_update.in says $K_SIZE"
	[ "$(md5 "$JOINED_KERNEL")" = "$K_MD5" ] || err "kernel: joined md5 does not match ota_update.in"
else
	err "kernel: no chunks"
fi

check_chunks rootfs.squashfs "$R_MD5" "$R_SIZE"
check_chunks xImage "$K_MD5" "$K_SIZE"

# Room for NAND bad blocks. The partition size mtdinfo reports is nominal;
# flash_erase erases the whole partition before nandwrite writes it, so a
# rootfs that only just fits leaves the device with no rootfs when one block
# is bad. The OTA updater cannot see this.
ROOTFS_BLOCKS=$((ROOTFS_PART / ERASE_BLOCK))
KERNEL_BLOCKS=$((KERNEL_PART / ERASE_BLOCK))
ROOTFS_USED=$(((R_SIZE + ERASE_BLOCK - 1) / ERASE_BLOCK))
KERNEL_USED=$(((K_SIZE + ERASE_BLOCK - 1) / ERASE_BLOCK))
ROOTFS_FREE=$((ROOTFS_BLOCKS - ROOTFS_USED))
KERNEL_FREE=$((KERNEL_BLOCKS - KERNEL_USED))

[ "$ROOTFS_FREE" -ge "$ROOTFS_RESERVE_PEB" ] ||
	err "rootfs leaves $ROOTFS_FREE erase block(s) free of $ROOTFS_BLOCKS; the minimum is $ROOTFS_RESERVE_PEB (1 MiB)"
[ "$ROOTFS_FREE" -ge "$ROOTFS_WARN_PEB" ] ||
	warn "rootfs leaves only $ROOTFS_FREE erase block(s) free; $ROOTFS_WARN_PEB or more is the comfortable margin"
[ "$KERNEL_FREE" -ge "$KERNEL_RESERVE_PEB" ] ||
	err "kernel leaves $KERNEL_FREE erase block(s) free of $KERNEL_BLOCKS; the minimum is $KERNEL_RESERVE_PEB"

KERNEL_VER="$(strings "$JOINED_KERNEL" | grep -m1 '^Linux-' | sed 's/^Linux-//' || true)"
[ -n "$KERNEL_VER" ] || err "cannot read the kernel version from the image"
say "  rootfs: $R_SIZE bytes, $ROOTFS_USED/$ROOTFS_BLOCKS blocks, $ROOTFS_FREE free"
say "  kernel: $K_SIZE bytes, $KERNEL_USED/$KERNEL_BLOCKS blocks, $KERNEL_FREE free"
say "  kernel version: ${KERNEL_VER:-unknown}"

ROOT="$TMP/root"
if unsquashfs -f -no-xattrs -d "$ROOT" "$JOINED_ROOTFS" >/dev/null 2>&1; then
	find "$ROOT" -type f -name '*.ko' | sort >"$TMP/modules"
	MODULE_COUNT=0
	MODULE_BAD=0
	while IFS= read -r ko; do
		[ -n "$ko" ] || continue
		MODULE_COUNT=$((MODULE_COUNT + 1))
		vermagic="$(strings "$ko" | grep -m1 'vermagic=' | sed -E 's/.*vermagic=([^ ]+).*/\1/' || true)"
		if [ -z "$vermagic" ]; then
			err "${ko#"$ROOT"/} has no vermagic"
		elif [ "$vermagic" != "$KERNEL_VER" ]; then
			MODULE_BAD=$((MODULE_BAD + 1))
			err "${ko#"$ROOT"/} is built for $vermagic, the kernel is $KERNEL_VER"
		fi
	done <"$TMP/modules"
	[ "$MODULE_COUNT" -gt 0 ] || warn "the rootfs has no .ko modules; the kernel/module check is empty"
	[ "$MODULE_COUNT" -eq 0 ] || say "  modules: $MODULE_COUNT checked, $MODULE_BAD mismatched"

	if [ -n "$PLAYER" ]; then
		[ -f "$PLAYER" ] || die "the expected player does not exist: $PLAYER"
		IMAGE_PLAYER="$ROOT/usr/bin/sonix_player"
		[ -f "$IMAGE_PLAYER" ] || err "usr/bin/sonix_player is missing"
		if [ -f "$IMAGE_PLAYER" ]; then
			[ "$(md5 "$IMAGE_PLAYER")" = "$(md5 "$PLAYER")" ] ||
				err "usr/bin/sonix_player is not the binary that was packed"
			[ -x "$IMAGE_PLAYER" ] || err "usr/bin/sonix_player is not executable"
			file "$IMAGE_PLAYER" | grep -q 'ELF 32-bit LSB' || err "usr/bin/sonix_player is not a 32-bit ELF"
			if [ -f "$ROOT/usr/bin/sonix_launch" ]; then
				grep -q 'exec /usr/bin/sonix_launch' "$ROOT/usr/bin/sonix_player.sh" ||
					err "sonix_launch is installed but sonix_player.sh does not exec it"
			fi
		fi
	fi

	if [ -n "$DEVICE" ]; then
		grep -q "\"device-name\"[[:space:]]*:[[:space:]]*\"$DEVICE\"" \
			"$ROOT/usr/resource/sonix/components/system-info.json" 2>/dev/null ||
			err "system-info.json does not say device-name \"$DEVICE\""
	fi
else
	err "cannot unpack the rootfs"
fi

if [ -f "$OTA/ota_compatibility.in" ]; then
	say "  compatibility: $(sed 's/^compatibility=//' "$OTA/ota_compatibility.in" | tr -d '\r\n')"
fi

if [ "$ERRORS" -ne 0 ]; then
	die "$ERRORS problem(s); this image must not be published"
fi

say "OK  $MODEL $UPT ($(size "$UPT") bytes)"
