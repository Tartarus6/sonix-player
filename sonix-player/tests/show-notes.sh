#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
binary=$(mktemp)
trap 'rm -f "$binary"' EXIT
# CFLAGS may be used to run with sanitizers.
${CC:-cc} ${CFLAGS:-} -std=gnu11 -Wall -Wextra -Werror -pthread -I. -I lvgl \
    tests/show-notes.c src/system/streaming/podcastrss.c src/system/streaming/podcastnotestext.c \
    src/system/ebook/epub_xml.c -o "$binary"
"$binary"
