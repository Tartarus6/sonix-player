#!/usr/bin/env python3
"""Render the engineering report Markdown to PDF with embedded screenshots."""

from __future__ import annotations

import argparse
from pathlib import Path

import markdown
from weasyprint import HTML

CSS = """
@page { size: A4; margin: 18mm 16mm; @bottom-center { content: counter(page); } }
body { font-family: "DejaVu Sans", sans-serif; font-size: 9.5pt; line-height: 1.35; color: #1a1a1a; }
h1 { font-size: 19pt; margin: 0 0 6pt 0; }
h2 { font-size: 13pt; margin: 16pt 0 5pt 0; border-bottom: 1px solid #999; }
h3 { font-size: 11pt; margin: 11pt 0 4pt 0; }
p, li { margin: 3pt 0; }
table { border-collapse: collapse; width: 100%; margin: 7pt 0; font-size: 8.2pt; }
th, td { border: 1px solid #bbb; padding: 3pt 4pt; text-align: left; vertical-align: top; }
th { background: #eee; }
code { font-family: "DejaVu Sans Mono", monospace; font-size: 8pt; background: #f3f3f3; }
pre { background: #f3f3f3; padding: 6pt; white-space: pre-wrap; font-size: 8pt; }
img { max-width: 92%; max-height: 115mm; display: block; margin: 8pt auto; }
blockquote { border-left: 3px solid #888; padding-left: 8pt; color: #444; }
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()

    text = args.source.read_text(encoding="utf-8")
    body = markdown.markdown(
        text,
        extensions=["tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"permalink": False}},
    )
    html = f"<!doctype html><html><head><meta charset='utf-8'><style>{CSS}</style></head><body>{body}</body></html>"
    HTML(string=html, base_url=str(args.source.resolve().parent)).write_pdf(str(args.output))
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
