#!/usr/bin/env bash
# Locate, verify and stage the proprietary stock firmware inputs used by the
# three firmware targets. Nothing here builds an image and nothing here can
# make one flashable: this is only an input-integrity gate.
#
# Expected digests come from docs/firmware/STOCK_SHA256.sha256 (values observed
# in the local tree, not invented). Missing or unverified input is a hard
# failure. The images themselves are never committed to git.
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: prepare_stock_inputs.sh --device r1|r3proii|v1|all --dest DIR [options]

  --stock-dir DIR   where r1_original.upt / r3proii_original.upt live
                    (default: $SONIX_STOCK_DIR or <repo>/sonix-packer)
  --v1-input PATH   V1 split parts, a joined archive, or a prepared v1_original.upt
                    (default: $SONIX_V1_INPUT or <repo>/sonix-packer/v1/firmware-input)
  --manifest FILE   sha256 manifest (default: <repo>/docs/firmware/STOCK_SHA256.sha256)

Extra stock search directories can be supplied with SONIX_STOCK_SEARCH_PATHS
(colon separated). The first existing match wins.
EOF
	exit 2
}

die() { echo "prepare_stock_inputs.sh: ERROR: $*" >&2; exit 1; }
note() { echo "prepare_stock_inputs.sh: $*"; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE=""
DEST=""
STOCK_DIR="${SONIX_STOCK_DIR:-$REPO/sonix-packer}"
V1_INPUT="${SONIX_V1_INPUT:-$REPO/sonix-packer/v1/firmware-input}"
MANIFEST="$REPO/docs/firmware/STOCK_SHA256.sha256"

while [ "$#" -gt 0 ]; do
	case "$1" in
		--device) shift; [ "$#" -gt 0 ] || usage; DEVICE="$1" ;;
		--dest) shift; [ "$#" -gt 0 ] || usage; DEST="$1" ;;
		--stock-dir) shift; [ "$#" -gt 0 ] || usage; STOCK_DIR="$1" ;;
		--v1-input) shift; [ "$#" -gt 0 ] || usage; V1_INPUT="$1" ;;
		--manifest) shift; [ "$#" -gt 0 ] || usage; MANIFEST="$1" ;;
		-h|--help) usage ;;
		*) usage ;;
	esac
	shift
done

[ -n "$DEVICE" ] && [ -n "$DEST" ] || usage
case "$DEVICE" in
	r1|r3proii|v1) DEVICES=("$DEVICE") ;;
	all) DEVICES=(r1 r3proii v1) ;;
	*) usage ;;
esac

for tool in sha256sum cat cp find sed awk; do
	command -v "$tool" >/dev/null 2>&1 || die "required tool missing: $tool"
done

[ -f "$MANIFEST" ] || die "SHA-256 manifest not found: $MANIFEST"

declare -A EXPECT=()
while IFS= read -r line; do
	line="${line%%#*}"
	[ -n "$line" ] || continue
	hash="$(printf '%s' "$line" | awk '{print $1}')"
	name="$(printf '%s' "$line" | awk '{print $2}')"
	[[ "$hash" =~ ^[0-9a-fA-F]{64}$ ]] || die "bad hash in $MANIFEST: $line"
	[ -n "$name" ] || die "bad entry in $MANIFEST: $line"
	EXPECT["$name"]="$(printf '%s' "$hash" | tr '[:upper:]' '[:lower:]')"
done < "$MANIFEST"

for name in r1_original.upt r3proii_original.upt v1.zip.part1 v1.zip.part2 v1.zip v1_original.upt; do
	[ -n "${EXPECT[$name]:-}" ] || die "manifest has no digest for $name"
done

mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sonix-stock.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

check_digest() {
	local file="$1" name="$2" want="${EXPECT[$2]}" got
	[ -f "$file" ] || die "$name not found (looked for: $file)"
	got="$(sha256sum "$file" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')"
	if [ "$got" != "$want" ]; then
		die "$name failed SHA-256 verification
  expected: $want
  observed: $got
  file:     $file
Refusing to package with an unverified stock input."
	fi
	note "verified $name: $got"
}

locate_stock() {
	local name="$1" dir candidate extra="${SONIX_STOCK_SEARCH_PATHS:-}"
	for dir in "$STOCK_DIR" ${extra//:/ }; do
		[ -n "$dir" ] || continue
		candidate="$dir/$name"
		if [ -f "$candidate" ]; then printf '%s\n' "$candidate"; return 0; fi
	done
	return 1
}

stage_hiby() {
	local dev="$1" name="${1}_original.upt" source target
	source="$(locate_stock "$name" || true)"
	[ -n "$source" ] || die "$name was not found in $STOCK_DIR (or SONIX_STOCK_SEARCH_PATHS).
Copy the proprietary stock firmware locally first; it is intentionally not in git."
	check_digest "$source" "$name"
	target="$DEST/$name"
	cp -f "$source" "$target"
	note "staged $dev stock input: $target"
}

stage_v1() {
	local part1 part2 joined candidate base
	# A single file may be the prepared stock UPT or an archive containing it.
	if [ -f "$V1_INPUT" ]; then
		base="$(basename "$V1_INPUT")"
		if [ "$base" = "v1_original.upt" ]; then
			check_digest "$V1_INPUT" v1_original.upt
			cp -f "$V1_INPUT" "$DEST/v1_original.upt"
			note "staged v1 stock input: $DEST/v1_original.upt"
			return 0
		fi
		command -v 7z >/dev/null 2>&1 || die "7z is required to read the V1 stock archive."
		"$REPO/sonix-packer/v1/prepare_v1_stock.sh" "$V1_INPUT" "$DEST" >&2
		check_digest "$DEST/v1_original.upt" v1_original.upt
		note "staged v1 stock input: $DEST/v1_original.upt"
		return 0
	fi

	candidate="$V1_INPUT/v1_original.upt"
	if [ -f "$candidate" ]; then
		if check_digest "$candidate" v1_original.upt; then
			cp -f "$candidate" "$DEST/v1_original.upt"
			note "staged v1 stock input: $DEST/v1_original.upt"
			return 0
		fi
		# A present but wrong file is not silently replaced by the parts.
		die "$candidate is present but did not match the recorded digest."
	fi

	part1="$V1_INPUT/v1.zip.part1"
	part2="$V1_INPUT/v1.zip.part2"
	[ -f "$part1" ] && [ -f "$part2" ] || die "V1 split parts not found in $V1_INPUT.
Expected v1.zip.part1 and v1.zip.part2 (official TempoTec V1 1.3 OTA)."
	check_digest "$part1" v1.zip.part1
	check_digest "$part2" v1.zip.part2

	joined="$WORK/v1.zip"
	cat "$part1" "$part2" > "$joined"
	check_digest "$joined" v1.zip

	command -v 7z >/dev/null 2>&1 || die "7z is required to extract v1.upt from the stock archive."
	"$REPO/sonix-packer/v1/prepare_v1_stock.sh" "$joined" "$DEST" >&2
	check_digest "$DEST/v1_original.upt" v1_original.upt
	note "staged v1 stock input: $DEST/v1_original.upt"
}

for dev in "${DEVICES[@]}"; do
	case "$dev" in
		r1|r3proii) stage_hiby "$dev" ;;
		v1) stage_v1 ;;
	esac
done

note "all requested stock inputs verified; output directory: $DEST"
