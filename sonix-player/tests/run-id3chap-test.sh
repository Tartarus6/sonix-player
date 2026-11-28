#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT
trap 'exit 1' HUP INT TERM

cd "$project_dir"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I. \
    tests/id3chap_test.c src/system/library/id3chap.c -o "$tmp_dir/id3chap_test"
"$tmp_dir/id3chap_test" "$tmp_dir/chapters.mp3"
