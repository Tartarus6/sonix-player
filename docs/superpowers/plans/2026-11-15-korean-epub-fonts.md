# Korean (Hangul) EPUB Font Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Hangul text render correctly in EPUBs on Sonix Player (HiBy R1/R3 Pro II) by adding script font fallbacks to the ebook font pipeline, and validate on HiBy R1.

**Architecture:** Keep Bookerly as the primary ebook typeface and append LVGL FreeType fallback chains (`lv_font_t->fallback`) for bundled script fonts (`korean`, `japanese`, `thai`, `arabic`) from `SONIX_RESOURCE_DIR "/fonts"`, so missing glyphs (Hangul) resolve even when EPUB metadata is wrong. Add `ko*` language detection and pass it through ebook font opening without changing the Japanese-book path.

**Tech Stack:** C, LVGL (v? in-tree `lvgl/`) with FreeType integration (`lv_freetype_font_create/delete`), Sonix Player ebook subsystem (`src/gui/ebook`, `src/system/ebook`), Makefile builds (`make host`, `make target`), HiBy R1 firmware packaging (`sonix-packer`, `r1.upt`).

**Spec:** `docs/superpowers/specs/2026-11-15-korean-epub-fonts-design.md`

## Global Constraints

- **Issue/behavior:** Fix [#190](https://github.com/Jepl4r/sonix-player/issues/190) — Hangul in EPUB must not render as `▯`; user-confirmed on HiBy R1.
- **Approach:** Fallback-by-glyph in ebook fonts (Bookerly primary for normal books) using bundled script fonts from `SONIX_RESOURCE_DIR "/fonts"`; keep Japanese-book behavior intact.
- **Device safety:** Test/build artifacts for HiBy R1 must target **`r1.upt` only** (never flash `r3proii.upt` to R1). Flash only with battery >30% and allow update to complete.
- **Scope/compat:** Minimal, localized changes in ebook font/language/reader plumbing; no unrelated refactors; avoid regressions for Latin and Japanese EPUBs.
- **Resources:** Prefer existing bundled fonts (`korean.otf`, `korean-bold.otf`, etc.); do not add new binary font assets unless strictly necessary (and if so, document license/size).
- **Work isolation:** Implement on a dedicated branch from current work; do not include unrelated local changes present in the repo.

## Review Focus

- **Hangul with `dc:language` missing/wrong or not `ko*`:** Bookerly lacks glyphs, so fallback must still render Hangul (do not rely solely on language flag).
- **Mixed content (Korean + Latin/numbers/punctuation) and styled runs:** regular/italic/bold/bold-italic spans must all resolve Hangul via appropriate fallback chains without crashes.
- **Font lifecycle on size change / reopen / close:** fallback fonts are size-dependent and must be recreated/destroyed in sync with primaries; no use-after-free or leaks when reader closes or font size changes.
- **Japanese books (`ja*`) and `ebook_is_vertical`:** Japanese primary/fallback behavior must remain correct; adding fallbacks must not break existing CJK/Japanese rendering path.
- **Resource/asset availability on device:** `korean.otf` (and bold) must exist in packaged `usr/resource/sonix/fonts/`; missing fallback font files must degrade gracefully (no crash), even if Hangul cannot render.

---

## File Structure

- **Modify:** `sonix-player/src/system/ebook/ebook.h` — declare `ebook_is_korean(const ebook_t *book)`.
- **Modify:** `sonix-player/src/system/ebook/ebook.c` — implement `ebook_is_korean()` (parallel to `ebook_is_japanese()`).
- **Modify:** `sonix-player/src/gui/ebook/ebookfonts.h` — extend `ebookfonts_open` signature to accept Korean hint: `bool ebookfonts_open(const char *sd_root, int size, bool japanese, bool korean);` and document fallback behavior.
- **Modify:** `sonix-player/src/gui/ebook/ebookfonts.c` — core fix: resolve/load script fallback fonts from `SONIX_RESOURCE_DIR "/fonts"`, build fallback chains (regular-preferred and bold-preferred), attach to primary faces on load, and manage fallback lifecycle in open/close/set_size.
- **Modify:** `sonix-player/src/gui/ebook/ebookreader.c` — call `ebookfonts_open(..., ebook_is_japanese(book), ebook_is_korean(book))`.
- **Verify (no code change expected):** `sonix-packer/assets/R1/usr/resource/sonix/fonts/korean.otf`, `korean-bold.otf` (and R3PII equivalents) present.
- **Create (local test artifact, not committed unless desired):** a minimal Korean EPUB sample for reproduction/regression during development (kept outside source or under a temp/test path as convenient).

Responsibilities: language detection stays in `system/ebook`; font loading/chaining stays in `gui/ebook/ebookfonts`; reader only orchestrates.

### Task 1: Isolate work and add Korean language detection

**Files:**
- Modify: `sonix-player/src/system/ebook/ebook.h`
- Modify: `sonix-player/src/system/ebook/ebook.c`

**Interfaces:**
- Consumes: existing `ebook_t` with `book->language` (as used by `ebook_is_japanese`).
- Produces: `bool ebook_is_korean(const ebook_t *book);` returning true for language tags starting with `ko` / `KO` followed by end, `-`, or `_` (e.g. `ko`, `ko-KR`, `ko_KR`).

- [ ] **Step 1: Create a dedicated branch for the fix**

Run (from `sonix-player/` repo root, `/home/diogo/Downloads/Hiby/sonix-player`):
```bash
git status -sb
git checkout -b fix/epub-korean-font-fallback
```
Expected: new branch created. If unrelated local changes would interfere, stash or keep them separate; do not commit them as part of this fix.

- [ ] **Step 2: Declare `ebook_is_korean` in the ebook API**

In `sonix-player/src/system/ebook/ebook.h`, near `ebook_is_japanese` / `ebook_is_vertical`, add:
```c
bool ebook_is_korean(const ebook_t *book);
```
(Keep comment style consistent: e.g. a book whose `dc:language` is Korean (`"ko"`), used to guide ebook font selection.)

- [ ] **Step 3: Implement `ebook_is_korean`**

In `sonix-player/src/system/ebook/ebook.c`, next to `ebook_is_japanese`, add:
```c
bool ebook_is_korean(const ebook_t *book) {
	const char *lang = book && book->language ? book->language : "";
	return (lang[0] == 'k' || lang[0] == 'K') && (lang[1] == 'o' || lang[1] == 'O') &&
		   (lang[2] == '\0' || lang[2] == '-' || lang[2] == '_');
}
```

- [ ] **Step 4: Verify it compiles (ebook translation unit / quick build check)**

Run (from `sonix-player/sonix-player`):
```bash
make -n host >/dev/null 2>&1 && echo "makefile ok"
```
If host toolchain/deps are available, compile at least the touched objects or run `make host` (expect no new errors from this change). If deps are missing, proceed but ensure syntax is correct (`gcc -fsyntax-only` not directly applicable due to project flags/includes; rely on later build).

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/system/ebook/ebook.h sonix-player/src/system/ebook/ebook.c
git commit -m "ebook: detect Korean language metadata"
```

### Task 2: Extend ebook font API and implement script fallback chains

**Files:**
- Modify: `sonix-player/src/gui/ebook/ebookfonts.h`
- Modify: `sonix-player/src/gui/ebook/ebookfonts.c`

**Interfaces:**
- Consumes: `SONIX_RESOURCE_DIR` (from `src/system/core/respath.h`), `lv_freetype_font_create/delete`, existing primary face loading (`EBOOK_FONT_DIR` Bookerly, `JAPANESE_FONT_DIR`/`JAPANESE_FILES` for Japanese books), `current_size`, `have_regular`, `face_or_load`.
- Produces:
  - `bool ebookfonts_open(const char *sd_root, int size, bool japanese, bool korean);` — opens primary faces as today and prepares script fallback fonts at `size`.
  - Primary faces returned by `ebookfonts_face()` have `fallback` chains attached so glyphs missing in primary resolve via script fonts (at least Korean) when available.
  - `ebookfonts_set_size()` and `ebookfonts_close()` rebuild/destroy fallback fonts consistently with primaries.

- [ ] **Step 1: Update header signature and contract**

In `sonix-player/src/gui/ebook/ebookfonts.h`:
- Change:
```c
bool ebookfonts_open(const char *sd_root, int size, bool japanese);
```
to:
```c
bool ebookfonts_open(const char *sd_root, int size, bool japanese, bool korean);
```
- Update the comment to state: `korean` hints a Korean book; regardless of hints, script fallback fonts (e.g. Korean, and other available scripts) are chained behind primary faces when present so missing glyphs (Hangul/CJK/scripts) can resolve.

- [ ] **Step 2: Add fallback font storage and helpers in `ebookfonts.c`**

Implement in `sonix-player/src/gui/ebook/ebookfonts.c` (keeping style consistent, `static` helpers):
- Define script font base dir as `SONIX_RESOURCE_DIR "/fonts"` (reuse `JAPANESE_FONT_DIR` or add `SCRIPT_FONT_DIR`).
- Define fallback script entries (order matters: try Korean early to satisfy #190; include others available like UI): e.g. Korean, Japanese, Thai, Arabic with regular/bold filenames (`"korean.otf"`, `"korean-bold.otf"`, etc.; follow existing naming in `fonts.c`/resources).
- Add storage for fallback `lv_font_t*` at current size, for two chains (regular-preferred and bold-preferred), e.g. arrays/lists sufficient to hold created faces and to know chain heads. Track which are loaded for current `size`.
- Add helpers:
  - `static lv_font_t *open_font_path(const char *path, int size)` (or reuse logic) that returns NULL if not readable/create fails.
  - `static void fallbacks_close(void)` deletes all fallback `lv_font_t` objects and clears chain heads.
  - `static void fallbacks_open(int size, bool japanese, bool korean)` (hints may influence order/priority but must not be required for Hangul coverage) that creates fallback faces for available script fonts and links `fallback` pointers into a regular chain and a bold chain. For each script, prefer bold file in bold chain when present else regular; prefer regular in regular chain when present else bold (or sensible fallback). Do not crash if files missing.
  - `static void attach_fallbacks(lv_font_t *face, bool bold_face)` sets `face->fallback = (bold_face ? bold_chain_head : regular_chain_head)` when head exists and `face` non-NULL.

Notes: keep implementation simple and deterministic; avoid UI-only Japanese CJK glyph routing complexity unless needed—glyph-miss fallback is sufficient for #190.

- [ ] **Step 3: Integrate fallback lifecycle into open/close/set_size/face loading**

- Change `ebookfonts_open` signature to `(const char *sd_root, int size, bool japanese, bool korean)`.
- In `ebookfonts_open`: after determining `current_size`/`face_files` and loading `FACE_REGULAR` (existing logic), call `fallbacks_close()` then `fallbacks_open(current_size, japanese, korean)` and attach fallbacks to any already-loaded primary faces (`faces[FACE_REGULAR]` etc.). Ensure Japanese-book mode (`face_files == JAPANESE_FILES`) still works; fallbacks should still be attached so Hangul in such books can resolve if needed.
- In `close_faces()` or `ebookfonts_close()`: call `fallbacks_close()` and ensure no primary face retains dangling `fallback` pointers (faces are deleted; clearing is implicit, but be orderly).
- In `ebookfonts_set_size()`: after closing/reopening primary faces at new size (existing `wanted` logic), reopen fallbacks for `current_size` and attach to each reopened primary face.
- In `face_or_load()` (lazy primary load): after successfully creating `faces[which]`, attach fallbacks appropriate to style (`FACE_BOLD`/`FACE_BOLD_ITALIC` -> bold-preferred, others -> regular-preferred) before returning.

- [ ] **Step 4: Sanity-check behavior when no fallback fonts exist**

Ensure that if no script font files are found/readable, `ebookfonts_face()` behavior remains as before (Bookerly/primary or UI fallback when `!have_regular`) and no NULL dereference occurs when attaching empty chains.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/gui/ebook/ebookfonts.h sonix-player/src/gui/ebook/ebookfonts.c
git commit -m "ebook: add script font fallbacks for Hangul and CJK"
```

### Task 3: Wire reader to Korean detection and verify assets

**Files:**
- Modify: `sonix-player/src/gui/ebook/ebookreader.c`
- Verify: `sonix-packer/assets/R1/usr/resource/sonix/fonts/korean.otf`, `sonix-packer/assets/R1/usr/resource/sonix/fonts/korean-bold.otf` (and R3PII)

**Interfaces:**
- Consumes: `ebook_is_japanese(book)`, `ebook_is_korean(book)`, updated `ebookfonts_open`.
- Produces: reader opens ebook fonts with both language hints.

- [ ] **Step 1: Update `ebookfonts_open` call site**

In `sonix-player/src/gui/ebook/ebookreader.c` (around line where open happens):
```c
ebookfonts_open(g_cfg->sd_root_path, opt_size, ebook_is_japanese(book), ebook_is_korean(book));
```
Confirm includes already cover `ebook.h` (they do; reader uses `ebook_is_japanese`).

- [ ] **Step 2: Verify packaged font assets exist**

Run (from `sonix-player/` repo root):
```bash
ls sonix-packer/assets/R1/usr/resource/sonix/fonts | grep -i korean
ls sonix-packer/assets/R3PII/usr/resource/sonix/fonts | grep -i korean
```
Expected: `korean.otf` and `korean-bold.otf` present for both. If missing from either, restore/add the expected files consistent with existing UI font packaging (do not invent new assets; copy from the correct existing source in repo if present elsewhere).

- [ ] **Step 3: Commit (if reader/assets changed)**

```bash
git add sonix-player/src/gui/ebook/ebookreader.c
# add assets only if actually modified/added
git commit -m "ebook: enable Korean font hint when opening reader"
```

### Task 4: Build and validate in host simulator

**Files:**
- No source changes expected (fix validation). Temporary local test EPUB (not committed).

**Interfaces:**
- Consumes: built `sonix_player_host`, resource tree containing `language/`, `fonts/` (including korean, default/bold), `fonts/ebook/` (Bookerly), `components/system-info.json`.
- Produces: evidence that Korean EPUB renders Hangul (no tofu) and Latin/Japanese EPUBs unaffected.

- [ ] **Step 1: Prepare a minimal Korean test EPUB and resources**

- Create or obtain a small EPUB containing Hangul text (e.g. `안녕하세요. 저는 sonix-player 이용자입니다.`) and mixed Latin/numbers; note if language metadata is `ko` or absent (ideally test at least one case where fallback-by-glyph matters).
- Ensure a resource tree is available next to host binary (e.g. copy `sonix-packer/assets/R1/usr/resource/` to `sonix-player/sonix-player/usr/resource/` or point as needed). Confirm `usr/resource/sonix/fonts/korean.otf` and `fonts/ebook/Bookerly-Regular.ttf` exist.

- [ ] **Step 2: Build host**

Run (from `sonix-player/sonix-player`):
```bash
make host -j$(nproc)
```
Expected: build succeeds (or identify and fix compile errors from Tasks 1-3). Note: host build needs SDL2/freetype/opusfile/wavpack/alsa dev packages.

- [ ] **Step 3: Run simulator and verify**

Run:
```bash
SONIX_SD_ROOT=/path/to/folder/with/test-epubs ./sonix_player_host
```
Verify:
- Open Korean EPUB: Hangul glyphs render (no `▯`), including mixed content and styled text where applicable.
- Open a known-good Latin EPUB: rendering unchanged.
- If available, open a Japanese EPUB: rendering unchanged.
- Change ebook font size and reopen/close book: no crash or visual corruption.

Record results (screenshots/notes) for the fix/PR and for user confirmation.

- [ ] **Step 4: Commit any necessary fixes from validation**

If validation required small code fixes, commit them with a clear message (amend or new commit as appropriate). Do not commit temporary test EPUBs or local resource copies.

### Task 5: Build `r1.upt`, flash and test on HiBy R1 (manual)

**Files:**
- Build artifacts: `sonix-player/sonix-player/sonix_player` (target), `sonix-packer/sonix_player`, `sonix-packer/r1.upt` (and only R1 as needed).

**Interfaces:**
- Consumes: successful target build, `sonix-packer/r1_original.upt` and assets (with korean fonts), packer script.
- Produces: `r1.upt` installed on HiBy R1 showing correct Hangul in EPUB.

- [ ] **Step 1: Build target binary**

Run (from `sonix-player/sonix-player`, with cross-toolchain and deps ready per README):
```bash
make target -j$(nproc)
```
Expected: `sonix_player` (and `sonix_launch` if built) produced, ABI check passes (no symbol beyond glibc 2.22).

- [ ] **Step 2: Package R1 firmware image**

From `sonix-player/` repo root (or as per local `build-r1-upt.sh`/README), place/copy required inputs into `sonix-packer/` (`sonix_player`, `r1_original.upt`, assets including korean fonts) and run:
```bash
cd sonix-packer
./sonix_firmware_packer.sh
ls -lh r1.upt
```
Expected: `r1.upt` generated for R1 only. Confirm it is the R1 image (not `r3proii.upt`).

- [ ] **Step 3: Flash to HiBy R1 (user action)**

- Ensure R1 battery **>30%** and a working microSD card.
- Copy **only `r1.upt`** to the **root** of the microSD card.
- On R1: `Settings > System > Update firmware > From SD card` (or stock update flow if applicable) and wait for “Upgrading…” → “Succeeded” → automatic reboot. Do not power off or remove card during update.
- Recovery if needed: flash official HiBy stock firmware via the same SD update procedure.

- [ ] **Step 4: Verify on device**

- Open the same Korean EPUB used in reproduction: Hangul must render correctly (no `▯`).
- Check mixed content and a couple of non-Korean EPUBs for regressions.
- Capture photo/screenshot and note firmware/build version tested.

- [ ] **Step 5: Finalize**

```bash
git status -sb
git log --oneline -n 5
```
Ensure only intended changes are committed on `fix/epub-korean-font-fallback`. Prepare summary/evidence for issue #190 / PR (device, version/build, before-after, test files used).

