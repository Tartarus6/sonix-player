#!/usr/bin/env bash
# Rebuild the EPUB script font subsets for both device models.
# Downloads the unhinted Noto Sans Regular faces, subsets each one to the
# codepoint ranges the reader needs, and installs the result into the assets.
# Also removes the stale bold faces so a rerun reproduces the shipped state.
# Requires: curl, hb-subset (HarfBuzz 15.0.0), coreutils.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

UPSTREAM="https://raw.githubusercontent.com/notofonts/notofonts.github.io/main/fonts"
CACHE_DIR=${EPUB_FONT_CACHE:-/tmp/epub-font-audit/unhinted}

FONT_MODELS=(R1 R3PII)
FONT_DIRS=(
	"$REPO_ROOT/sonix-packer/assets/R1/usr/resource/sonix/fonts"
	"$REPO_ROOT/sonix-packer/assets/R3PII/usr/resource/sonix/fonts"
)

# output name | Noto family | Unicode ranges
SCRIPTS=(
	"devanagari|Devanagari|A0,900-97F,2000-206F"
	"tamil|Tamil|A0,B80-BFF,2000-206F"
	"malayalam|Malayalam|A0,D00-D7F,2000-206F"
	"gurmukhi|Gurmukhi|A0,A00-A7F,2000-206F"
	"ethiopic|Ethiopic|A0,1200-139F,2000-206F,2D80-2DDF,AB00-AB2F"
	"hebrew|Hebrew|A0,590-5FF,2000-206F,FB1D-FB4F"
)

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

for entry in "${SCRIPTS[@]}"; do
	name=${entry%%|*}
	rest=${entry#*|}
	family=${rest%%|*}
	ranges=${rest#*|}

	src="$WORK_DIR/${family}-Regular.ttf"
	url="$UPSTREAM/NotoSans${family}/unhinted/ttf/NotoSans${family}-Regular.ttf"
	cached="$CACHE_DIR/${name}-Regular.ttf"

	if ! curl -fsSL --max-time 60 -o "$src" "$url" || [ ! -s "$src" ]; then
		echo "unhinted source unavailable for $name, using cache: $cached" >&2
		cp "$cached" "$src"
	fi

	printf '%s\n' "$ranges" > "$WORK_DIR/${name}.unicodes"
	hb-subset --unicodes-file="$WORK_DIR/${name}.unicodes" -o "$WORK_DIR/${name}.ttf" "$src"
done

for i in "${!FONT_DIRS[@]}"; do
	dir=${FONT_DIRS[$i]}
	for entry in "${SCRIPTS[@]}"; do
		name=${entry%%|*}
		cp "$WORK_DIR/${name}.ttf" "$dir/${name}.ttf"
		rm -f "$dir/${name}-bold.ttf"
	done
done

for i in "${!FONT_DIRS[@]}"; do
	dir=${FONT_DIRS[$i]}
	model=${FONT_MODELS[$i]}
	total=0
	for entry in "${SCRIPTS[@]}"; do
		name=${entry%%|*}
		total=$((total + $(stat -c%s "$dir/${name}.ttf")))
	done
	echo "$model: $total bytes"
done
