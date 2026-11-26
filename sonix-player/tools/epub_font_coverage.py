#!/usr/bin/env python3
"""Report characters in an EPUB that no font in a folder can draw.

The EPUB reader draws every letter from a font chain; a codepoint missing
from all of them becomes a tofu box. This checks the text of every XHTML
document in an EPUB against the union of the cmaps of the font files given
(on the device those are the files under usr/resource/sonix/fonts).

    python3 epub_font_coverage.py --epub book.epub --fonts fonts/

Exits 0 when every non-ASCII, non-whitespace character is covered, 1 when
any is missing. Needs fontconfig's fc-query on PATH.
"""

import argparse
import html
import re
import shutil
import subprocess
import sys
import zipfile
from html.parser import HTMLParser
from pathlib import Path

HEX_RANGE_RE = re.compile(r"^([0-9a-fA-F]+)(?:-([0-9a-fA-F]+))?$")


class VisibleText(HTMLParser):
    """Collects text nodes, leaving out script and style bodies."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.skipping = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.skipping += 1

    def handle_endtag(self, tag):
        if tag in ("script", "style") and self.skipping:
            self.skipping -= 1

    def handle_data(self, data):
        if not self.skipping:
            self.parts.append(data)

    def text(self):
        return "".join(self.parts)


def text_of(markup):
    parser = VisibleText()
    parser.feed(markup)
    parser.close()
    return html.unescape(parser.text())


def charset_of(path):
    result = subprocess.run(
        ["fc-query", "-f", "%{charset}", str(path)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"fc-query failed for {path}: {result.stderr.strip()}")
    covered = set()
    for token in result.stdout.split():
        match = HEX_RANGE_RE.match(token)
        if not match:
            continue
        first = int(match.group(1), 16)
        last = int(match.group(2), 16) if match.group(2) else first
        covered.update(range(first, last + 1))
    return covered


def chapters(epub):
    """Yields (label, text) for every XHTML document in the book."""
    with zipfile.ZipFile(epub) as archive:
        for name in sorted(archive.namelist()):
            if not name.lower().endswith((".xhtml", ".html", ".htm")):
                continue
            raw = archive.read(name)
            text = text_of(raw.decode("utf-8", errors="replace"))
            label = Path(name).stem
            yield label, text


def font_files(root):
    root = Path(root)
    if not root.is_dir():
        raise SystemExit(f"not a folder: {root}")
    files = sorted(
        path
        for path in root.rglob("*")
        if path.suffix.lower() in (".ttf", ".otf", ".ttc") and path.is_file()
    )
    if not files:
        raise SystemExit(f"no font files under {root}")
    return files


def main():
    parser = argparse.ArgumentParser(
        description="Check EPUB text against the glyph coverage of a font folder."
    )
    parser.add_argument("--epub", required=True, type=Path)
    parser.add_argument("--fonts", required=True, type=Path)
    args = parser.parse_args()

    if shutil.which("fc-query") is None:
        raise SystemExit("fc-query is not on PATH (install fontconfig)")
    if not args.epub.is_file():
        raise SystemExit(f"not a file: {args.epub}")

    covered = set()
    for path in font_files(args.fonts):
        covered |= charset_of(path)

    missing_chapters = 0
    for label, text in chapters(args.epub):
        needed = {ord(char) for char in text if ord(char) > 0x7F and not char.isspace()}
        missing = sorted(needed - covered)
        if not missing:
            print(f"{label}: ok")
            continue
        missing_chapters += 1
        shown = " ".join(f"U+{cp:04X}" for cp in missing)
        print(f"{label}: {len(missing)} missing  {shown}")

    if missing_chapters:
        print(f"{missing_chapters} chapters with missing glyphs")
        return 1
    print("all chapters covered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
