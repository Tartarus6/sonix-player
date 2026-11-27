#!/bin/bash
#
# Test that a failed verification leaves no .upt at the output path.
# Sources a copy of the packer with the build loop removed, so no stock
# firmware, toolchain or device is needed.
#
# usage: ./tests/test_packer_failure.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKER="$HERE/../sonix_firmware_packer.sh"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/packer-failure-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/sonix-packer/assets/R1"
: >"$TMP/sonix-packer/r1_original.upt"
cp "$PACKER" "$TMP/sonix-packer/"
cp "$HERE/../base.lock" "$HERE/../image.lock" "$HERE/../kernel.lock" "$HERE/../verify_upt.sh" "$TMP/sonix-packer/"
printf '#!/bin/sh\n' >"$TMP/sonix-packer/sonix_player"
chmod +x "$TMP/sonix-packer/sonix_player"
BASE_SUM="$(md5sum "$TMP/sonix-packer/r1_original.upt" | awk '{print $1}')"
printf '%s|r1_original.upt|Linux-4.4.94-sonix|test base\n' "$BASE_SUM" >>"$TMP/sonix-packer/base.lock"

python3 - "$TMP/sonix-packer/sonix_firmware_packer.sh" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
marker = "# ==========================================================================\n# Every model that can be built"
if marker not in text:
    raise SystemExit("FAIL: build loop marker missing from the packer")
open(path, "w").write(text[:text.index(marker)])
PY

# shellcheck source=/dev/null
source "$TMP/sonix-packer/sonix_firmware_packer.sh"

printf 'rejected image' >"$TMP/r1.upt"
discard_failed_image "$TMP/r1.upt"
[ ! -e "$TMP/r1.upt" ] || {
	echo "FAIL: a rejected image was left at the output path"
	exit 1
}

grep -Fq 'discard_failed_image "$OUTPUT_UPT"' "$PACKER" || {
	echo "FAIL: the packer does not discard the image when verification fails"
	exit 1
}

echo "packer failure tests: ok"
