# Korean (Hangul) EPUB font support — short design

**Issue:** [Jepl4r/sonix-player#190](https://github.com/Jepl4r/sonix-player/issues/190) — Korean characters in EPUB render as `▯` on HiBy R1 (Sonix Player 1.3.0). Reproduction confirmed by user on HiBy R1.

## Intent / why
EPUB reader loads only Bookerly (`fonts/ebook`) for normal books and only Japanese faces for Japanese books (`ebookfonts_open(..., ebook_is_japanese(book))` in `src/gui/ebook/ebookreader.c`, implementation in `src/gui/ebook/ebookfonts.c`). There is no Hangul coverage or glyph fallback, so Hangul renders as tofu even though UI fonts include `korean.otf` (`src/gui/fonts/fonts.c`). Fix by adding script font fallbacks in the ebook font pipeline.

## Goals
- Hangul (and mixed Korean+Latin/numbers) renders correctly in EPUBs.
- No visible regression for Latin and Japanese EPUBs.
- Robust when EPUB language metadata is missing/incorrect (glyph fallback).

## Non-goals
- Full guarantee for every script beyond what bundled fonts cover (though fallback may help). Embedded EPUB fonts: follow-up if current pipeline does not support them.

## Approach (1, chosen)
- `src/gui/ebook/ebookfonts.c/.h`: keep Bookerly primary for normal books; chain script fallback fonts from `SONIX_RESOURCE_DIR "/fonts"` via `lv_font_t->fallback` (at least `korean`/`korean-bold`, also `japanese`, `thai`, `arabic` when available, mirroring UI coverage). Attach fallbacks to every primary face variant (regular/italic/bold/bold-italic), preferring bold fallbacks for bold styles. Manage fallback font lifecycle across open/close/set_size (size-dependent, lazy primary loading).
- `src/system/ebook/ebook.c/.h`: add `ebook_is_korean()` (`ko*`).
- `src/gui/ebook/ebookreader.c`: pass Korean detection (and/or rely on always-on fallback) without breaking Japanese path.
- `sonix-packer/assets/.../usr/resource/sonix/fonts/`: verify `korean.otf`/`korean-bold.otf` present.

## Constraints / risks
- HiBy R1 memory/perf: limit fallback faces loaded, validate on device.
- Font metrics differences may affect pagination/line layout — visual validation required.
- Isolate work on a dedicated branch (repo currently on `wifi-manual-network` with unrelated local changes).

## Validation
1. Host simulator (`make host` in `sonix-player/sonix-player/`) with Korean EPUB + Latin/Japanese regression using proper resource tree.
2. Build `r1.upt` (target + packer, **never `r3proii.upt`**) and test on HiBy R1 via SD card update (battery >30%).
3. Optional stock firmware comparison on same file.
