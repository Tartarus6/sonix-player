# Album Collections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add named collections of whole albums to Sonix Player: create/rename/delete, add albums from the Albums list or Now Playing, browse a collection as albums, remove albums, and play the collection in order or shuffled.

**Architecture:** Two new on-card SQLite tables (`SONIX_COLLECTION`, `SONIX_COLLECTION_ITEM`) hold collections and album references with snapshots. A pure resolver (`collections_resolve.c`) implements the identity cascade and is unit-tested. A system module (`collections.c`) wires the resolver to the library and exposes a small API. Two new LVGL pages (list + windowed album detail) consume that API; the existing Albums selection and Now Playing menu get small hooks.

**Tech Stack:** C, LVGL, SQLite (the player's `library.db`), SDL host simulator, MIPS cross-build.

**Spec:** `docs/superpowers/specs/2026-11-16-album-collections-design.md`

---

## Global Constraints

- Album identity is `<album name>\x1f<key>`; never invent a stable album id.
- Never attach a collection item to an album unless the cascade is unique and corroborated. No "unique name" fallback.
- Album/artist/folder comparisons use `fold_text()` folding; collection-name uniqueness uses SQLite `COLLATE NOCASE`.
- The database is per card at `<card>/.local/library.db`; collections live in it, not in config, not in `MEDIA_TABLE`.
- New tables are `SONIX_COLLECTION` and `SONIX_COLLECTION_ITEM`, added to `KEPT_TABLES`.
- Only the GUI thread touches LVGL. Workers use `thread_be_low_priority()` and `gui_post()`.
- No stack allocation of multi-kilobyte collections; page reads are bounded.
- Every write is in a savepoint; a failed multi-add leaves no partial rows.
- All user-visible text goes through `tr()`; add keys to all 12 languages in both `R1` and `R3PII` trees.
- Use tabs, `clang-format` (LLVM, tabs, width 4, column limit 1000), no new `-Wall` warnings.
- No backup/import, no reorder, no streaming albums, no album-page action in v1.
- Album value buffers become `LIBRARY_ALBUM_VALUE_MAX` (320); `coverflow.c`'s 192-byte row name is out of scope.

---

## Review Focus

These are the inputs/conditions most likely to bite a user. Each is pinned by a test in the task that owns the code.

1. **Retagged or renamed album** (case, accent, album artist, `join_albums`): the collection follows the album or marks it missing; it never opens a different album with the same name. — Task 2 tests; Task 9 retag fixtures.
2. **Two albums share a name** (different artists/folders): adding and opening each keeps them distinct; selection never collapses them. — Task 2 tests; Task 9 fixtures.
3. **Card pulled, card swapped, database corrupted**: collections survive a rescan and a damaged-db rescue; pages do not use stale handles or crash. — Task 3 rescue/schema checks; Task 9 integration.
4. **Very long album name (near 255 bytes), Unicode, multi-disc, CUE**: the key survives every buffer; open and play work from the collection. — Task 1 grep; Task 9 fixtures.
5. **Huge selection or collection**: memory stays bounded, the UI stays responsive, and a failed add leaves no partial collection. — Task 3 writer API; Task 9 stress check.

---

## File Structure

**New system files**

- `src/system/library/collections_resolve.h` — pure resolver types and interface.
- `src/system/library/collections_resolve.c` — the identity cascade; no SQLite, no GUI.
- `src/system/library/collections.h` — public collections API.
- `src/system/library/collections.c` — name rules, resolver wiring, add/page/play paths.

**New GUI files**

- `src/gui/library/collectionspage.h` / `.c` — collections list and picker.
- `src/gui/library/collectionpage.h` / `.c` — one collection's albums, windowed.

**New test**

- `tests/collections_resolve_test.c` — host-only unit tests for the resolver.

**New asset**

- `assets/icons/collection.png` — Music tile artwork (optional fallback: `icon_menu_album`).

**Modified**

- `sonix-player/Makefile` — `test-collections` target.
- `src/system/library/library.h` / `library.c` — schema, rescue, revision, album snapshot/value APIs, collection SQL primitives, `LIBRARY_ALBUM_VALUE_MAX`.
- `src/gui/library/medialist.c` — album-value buffers, selection "Add to collection" hook.
- `src/gui/nowplaying/trackmenu.c` — album-value buffers, "Add album to collection" item.
- `src/system/playback/playlist.c`, `src/system/playback/device_state.c` — album-value buffers.
- `src/gui/library/music.h` / `music.c` — Collections tile.
- `src/gui/shell/gui.c` — initialize the two new screens.
- `src/gui/nowplaying/coverloader.h` — collection cover slots.
- `src/gui/shell/icons.c` / `icons.h` — regenerated from the SVG.
- `sonix-packer/assets/R1/.../language/*.ini`, `.../R3PII/.../language/*.ini` — 24 files.
- `FEATURES.md` — user-facing bullet.

---

## Execution Prerequisites

- Install the host build dependencies once:
  - Fedora: `sudo dnf install opusfile-devel wavpack-devel`
  - Debian/Ubuntu: `sudo apt install libopusfile-dev libwavpack-dev`
  - Verify: `pkg-config --modversion opusfile wavpack` prints two versions.
- Create an isolated worktree with the `using-git-worktrees` skill before Task 1.
  The main tree has unrelated uncommitted changes, including 24 language files;
  do not mix them into this feature.
- The design and this plan are untracked in the main tree. Copy
  `docs/superpowers/specs/2026-11-16-album-collections-design.md` and
  `docs/superpowers/plans/2026-11-16-album-collections.md` into the worktree and
  commit them (`docs: album collections design and plan`) before Task 1, so the
  spec travels with the branch.

---

## Task 1: Widen album value buffers

**Files:**
- Modify: `sonix-player/src/system/library/library.h`
- Modify: `sonix-player/src/system/library/library.c`
- Modify: `sonix-player/src/gui/library/medialist.c`
- Modify: `sonix-player/src/gui/nowplaying/trackmenu.c`
- Modify: `sonix-player/src/system/playback/playlist.c`
- Modify: `sonix-player/src/system/playback/device_state.c`
- Test: host build + grep + simulator smoke

**Interfaces:**
- Produces: `#define LIBRARY_ALBUM_VALUE_MAX 320` in `library.h`.
- Produces: every album-value buffer in the listed files uses that constant.

- [ ] **Step 1: Add the constant**

In `library.h`, beside `LIBRARY_ALBUM_KEY_SEP`, add:

```c
// The longest album value: a 255-byte album name, the separator, a 9-character
// key and the terminator. Buffers that hold a whole value use this, not 256.
#define LIBRARY_ALBUM_VALUE_MAX 320
```

- [ ] **Step 2: Replace the album-value buffers**

Use `LIBRARY_ALBUM_VALUE_MAX` for:

- `library.h`: `library_index_spec_t.value`.
- `library.c`: `struct library_index.value`; the `album` local in `library_index_find_name()`.
- `medialist.c`: `winrow_t.name`, `panel_step_t.value`, `panel_t.filter_value`, `menu_album`, `np_album`, the row-click `name` local, and the album-head `album` local.
- `trackmenu.c`: the `album` local in `open_album_cb()` and in the new collection action.
- `playlist.c`: `queue_album`, `persisted_album`.
- `device_state.c`: the `album` locals used to save/restore the queue album.

Do not change buffers that hold only a track name, an artist, a playlist title, or a path. Do not change `coverflow.c`'s `cf_row_t.name[192]`.

- [ ] **Step 3: Prove no album-value buffer is left at 256**

Run:

```bash
cd sonix-player/sonix-player
grep -n "char .*album.*\[256\]\|char value\[256\]\|char name\[256\]" \
  src/system/library/library.h src/system/library/library.c \
  src/gui/library/medialist.c src/gui/nowplaying/trackmenu.c \
  src/system/playback/playlist.c src/system/playback/device_state.c
```

Expected: only buffers that are not whole album values remain (for example a plain track `name[256]`). The list is reviewed in the commit message.

- [ ] **Step 4: Build the host**

Run: `cd sonix-player/sonix-player && make host -j$(nproc)`
Expected: `Host build complete`, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/system/library/library.h sonix-player/src/system/library/library.c \
  sonix-player/src/gui/library/medialist.c sonix-player/src/gui/nowplaying/trackmenu.c \
  sonix-player/src/system/playback/playlist.c sonix-player/src/system/playback/device_state.c
git commit -m "library: carry whole album values in 320-byte buffers"
```

---

## Task 2: Pure identity resolver and its tests

**Files:**
- Create: `sonix-player/src/system/library/collections_resolve.h`
- Create: `sonix-player/src/system/library/collections_resolve.c`
- Create: `sonix-player/tests/collections_resolve_test.c`
- Modify: `sonix-player/Makefile`

**Interfaces:**
- Produces: `collection_ref_t`, `collection_snapshot_t`, `collection_lookup_t`,
  `collection_resolve_step_t`, `collection_resolve()` exactly as below.
- Consumes: nothing from the library; the resolver is pure C.

```c
#define COLLECTION_ALBUM_MAX 256
#define COLLECTION_KEY_MAX 16
#define COLLECTION_ARTIST_MAX 256
#define COLLECTION_FOLDER_MAX 1024
#define COLLECTION_PATH_MAX 1024

typedef struct {
    char album[COLLECTION_ALBUM_MAX];
    char album_key[COLLECTION_KEY_MAX];
    char artist[COLLECTION_ARTIST_MAX];
    char folder[COLLECTION_FOLDER_MAX];
    char first_path[COLLECTION_PATH_MAX];
} collection_ref_t;

typedef collection_ref_t collection_snapshot_t;

typedef bool (*collection_snapshot_cb)(const collection_snapshot_t *snapshot, void *user);

typedef struct {
    bool (*snapshot)(const char *value, collection_snapshot_t *out, void *user);
    int (*named)(const char *name, collection_snapshot_cb cb, void *user);
    int (*keyed)(const char *key, collection_snapshot_cb cb, void *user);
    bool (*value_for_path)(const char *path, char *out, size_t out_size, void *user);
    void (*fold)(const char *in, char *out, size_t size);
} collection_lookup_t;

typedef enum {
    COLLECTION_RESOLVE_EXACT = 0,
    COLLECTION_RESOLVE_KEY,
    COLLECTION_RESOLVE_PATH,
    COLLECTION_RESOLVE_NAME_ARTIST,
    COLLECTION_RESOLVE_NAME_FOLDER,
    COLLECTION_RESOLVE_MISSING,
} collection_resolve_step_t;

collection_resolve_step_t collection_resolve(const collection_ref_t *ref,
                                               const collection_lookup_t *lookup,
                                               void *user,
                                               collection_snapshot_t *out);
```

- [ ] **Step 1: Write the failing tests**

Create `tests/collections_resolve_test.c` with a small `CHECK` macro and a fake
lookup backed by an array of snapshots (at most 16), a path-to-value map, and a
test `fold` that lowercases and maps `á é í ó ú ü ñ` to their base letters.
Cover these cases, one `CHECK` each:

1. `exact value resolves` → `COLLECTION_RESOLVE_EXACT`.
2. `same key, one candidate, same folded album name, matching artist resolves`
   → `KEY`, using a case/accent variant of the stored name.
3. `same key, one candidate, no corroboration is missing` → `MISSING`.
4. `same key, one candidate, different album name, matching artist is missing`
   → `MISSING` (wrong-album regression).
5. `same key, two candidates, one with the stored folded name and corroboration
   resolves` → `KEY`; the other candidate has a different name.
6. `first path resolves a renamed album with the same artist` → `PATH`.
7. `first path resolves a moved album with the same folder` → `PATH`.
8. `first path to an unrelated album is missing` → `MISSING`.
9. `name plus folded artist resolves` → `NAME_ARTIST`.
10. `name plus artist with two matches is missing` → `MISSING`.
11. `name plus folder resolves uniquely` → `NAME_FOLDER`.
12. `a single same-named album with a different artist is missing` → `MISSING`
    (proves the unique-name fallback is gone).
13. `accent-folded name and artist resolve` → `NAME_ARTIST`/`KEY`.
14. `an item with no key is missing` → `MISSING`.
15. `exact wins over every fallback` → `EXACT`.

Include the exact fixtures in the test: the duplicate-name albums, the
wrong-album candidate (same key and artist, different name), the accent pair,
and the unrelated album.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd sonix-player/sonix-player && make test-collections`
Expected: link or compile failure because `collections_resolve.c` does not exist.

- [ ] **Step 3: Implement the resolver**

In `collections_resolve.c`:

- If `ref`, `lookup`, `lookup->snapshot`, `lookup->named`, `lookup->keyed`,
  `lookup->value_for_path` or `lookup->fold` is null, or `ref->album[0]` or
  `ref->album_key[0]` is empty, return `COLLECTION_RESOLVE_MISSING`.
- Build the exact value with `snprintf` into a `char value[320]`, using
  `LIBRARY_ALBUM_KEY_SEP`; call `lookup->snapshot`; if true, copy to `out` and
  return `EXACT`.
- **Key step:** call `lookup->keyed(ref->album_key, ...)`; count only candidates
  whose album name folds to the stored name and which are corroborated. Accept
  only if exactly one qualifies; zero or multiple matches continue through the
  remaining fallbacks. Corroboration: folded artist equal (both non-empty), or
  folded folder equal (both non-empty), or both snapshots empty and folded title
  equal. A different album name never qualifies, even when key and artist match.
- **Path step:** if `ref->first_path[0]`, call `lookup->value_for_path`; on
  success call `lookup->snapshot` on the returned value; accept only if
  corroborated.
- **Name+artist step:** only if `ref->artist[0]`; call `lookup->named(ref->album,
  ...)`; count candidates whose folded artist equals the folded stored artist;
  accept only if exactly one.
- **Name+folder step:** only if `ref->folder[0]`; call `lookup->named`; count
  candidates whose folded folder equals the folded stored folder; accept only if
  exactly one.
- Return `MISSING` otherwise. Never write `out` on a miss.

The `fold` callback must be pure (no database) because the real implementation
runs candidate callbacks under the index lock.

- [ ] **Step 4: Add the Makefile target**

Add to `sonix-player/Makefile`, near the other `PHONY` targets:

```make
TEST_COLLECTIONS_BIN = build_host/collections_resolve_test

$(TEST_COLLECTIONS_BIN): tests/collections_resolve_test.c src/system/library/collections_resolve.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -o $@ $^

test-collections: $(TEST_COLLECTIONS_BIN)
	./$(TEST_COLLECTIONS_BIN)

.PHONY: test-collections
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd sonix-player/sonix-player && make test-collections`
Expected: every `CHECK` passes; exit 0.

- [ ] **Step 6: Commit**

```bash
git add sonix-player/src/system/library/collections_resolve.h \
  sonix-player/src/system/library/collections_resolve.c \
  sonix-player/tests/collections_resolve_test.c sonix-player/Makefile
git commit -m "collections: resolve album identity with a tested cascade"
```

---

## Task 3: Database tables and library primitives

**Files:**
- Modify: `sonix-player/src/system/library/library.h`
- Modify: `sonix-player/src/system/library/library.c`
- Test: host build + schema smoke + SQLite inspection

**Interfaces:**
- Adds `library_album_snapshot_t`, `library_album_snapshot()`,
  `library_album_values_named()`, `library_album_values_with_key()`,
  `library_fold_text()`, `library_collection_*` and
  `library_collections_revision()` exactly as the spec §9 defines.
- Consumes: the schema and existing `ALBUM_*` macros in `library.c`.
- Produces: the SQL and album-lookup layer that Task 4 wraps.

- [ ] **Step 1: Add the schema**

In `library.c`, append to `SCHEMA[]`:

```sql
CREATE TABLE IF NOT EXISTS SONIX_COLLECTION(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL COLLATE NOCASE UNIQUE,
    created INT);
CREATE TABLE IF NOT EXISTS SONIX_COLLECTION_ITEM(
    collection INT NOT NULL,
    idx INT NOT NULL,
    album TEXT NOT NULL COLLATE NOCASE,
    album_key TEXT NOT NULL,
    artist TEXT,
    folder TEXT,
    first_path TEXT,
    PRIMARY KEY(collection, idx),
    UNIQUE(collection, album, album_key));
```

Add to `SCHEMA_INDEXES[]`:

```sql
CREATE INDEX IF NOT EXISTS sonix_collection_item_album_idx
    ON SONIX_COLLECTION_ITEM(album, album_key);
```

Add `"SONIX_COLLECTION"` and `"SONIX_COLLECTION_ITEM"` to `KEPT_TABLES`.
Add `GEN_COLLECTIONS` before `GEN_COUNT` and extend the `generation[]`
initializer to four values.

- [ ] **Step 2: Add the album snapshot and candidate queries**

Implement in `library.c`, using the existing `ALBUM_VALUE_MATCH`,
`ALBUM_GROUP_VALUE` and `ALBUM_GROUP_FIRST` macros:

- `library_album_snapshot()`: one first-track row by album value; fill title,
  key, artist, first_path, folder (dirname of first_path). False when absent.
- `library_album_values_named()`: exact `album = ?1` first, then
  `foldcase(album) = foldcase(?1)` only if the first returned zero; each row is
  a full `library_album_snapshot_t`; callbacks run under `db_lock`.
- `library_album_values_with_key()`: `WHERE album_key = ?1`, full snapshots.
- `library_fold_text()`: a thin wrapper around `fold_text()`.

- [ ] **Step 3: Add the collection SQL primitives**

Implement the primitives from spec §9, with these exact behaviours:

- `create`: insert; duplicate name or SQL failure → `-1`; success → new id.
- `exists`: return id under `NOCASE`.
- `rename`: reject a name owned by another id; renaming to the current name
  succeeds; bump revision.
- `delete`: savepoint; delete items; delete row; release/rollback; bump.
- `list`: callback gets id, name and `COUNT(*)` of items, ordered by
  `name COLLATE listorder`; callback runs under `db_lock`.
- `count`: `COUNT(*)`.
- `page`: `ORDER BY idx LIMIT ? OFFSET ?` into a caller-owned array; release the
  lock before returning.
- `write_begin` / `write_row` / `write_end`: one savepoint, `MAX(idx)+1` once,
  `INSERT OR IGNORE`, `sqlite3_changes()`; one item in memory; duplicate is
  skipped, SQL error fails the writer and rolls back. `write_begin` returns
  NULL when the collection id does not exist.
- `update_item`: if another position already holds `(album, album_key)`, delete
  this position and return `MERGED`; otherwise update and return `UPDATED`;
  SQL failure returns `FAILED`. Bump revision only on success/merge.
- `remove`: delete the requested `idx` values in one savepoint; bump revision.
- `revision`: read `generation[GEN_COLLECTIONS]` under `db_lock`.

All answer `false` / `-1` / `0` when `db == NULL`.

- [ ] **Step 4: Build and schema smoke test**

Run:

```bash
cd sonix-player/sonix-player
make host -j$(nproc)
make test-collections
```

Expected: both pass.

Then launch the host against a fake card for a few seconds and inspect the
database:

```bash
mkdir -p /tmp/opencode/albumcol-sd/.local /tmp/opencode/albumcol-res
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
SONIX_SD_ROOT=/tmp/opencode/albumcol-sd \
SONIX_CONFIG=/tmp/opencode/albumcol-config.ini \
SONIX_LOG=/tmp/opencode/albumcol.log \
SONIX_NO_MOUNT=1 SONIX_NO_SYSSERVER=1 \
timeout 15s ./sonix_player_host
DB=/tmp/opencode/albumcol-sd/.local/library.db
[ -f "$DB" ] || DB=/tmp/.local/library.db
sqlite3 "$DB" '.schema SONIX_COLLECTION'
sqlite3 "$DB" '.schema SONIX_COLLECTION_ITEM'
sqlite3 "$DB" 'PRAGMA integrity_check;'
```

Expected: both tables and the index are created; `integrity_check` says `ok`.
The database is under the fake card when `/mnt/sd_0` is writable, and under
`/tmp/.local` otherwise; the fallback line selects whichever exists.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/system/library/library.h sonix-player/src/system/library/library.c
git commit -m "library: add album collection tables and primitives"
```

---

## Task 4: Public collections module

**Files:**
- Create: `sonix-player/src/system/library/collections.h`
- Create: `sonix-player/src/system/library/collections.c`
- Test: host build + resolver tests

**Interfaces:**
- Adds `COLLECTION_ALBUM_MAX`, `COLLECTION_KEY_MAX`, `COLLECTION_ARTIST_MAX`,
  `COLLECTION_FOLDER_MAX`, `COLLECTION_PATH_MAX` in `collections.h` (or reuses
  the values from `collections_resolve.h`).
- Includes `collections_resolve.h` and uses `collection_ref_t` /
  `collection_snapshot_t`.
- Produces public `collections_*` functions exactly as spec §9 defines.
- Consumes: `library_album_snapshot()`, `library_album_values_named()`,
  `library_album_values_with_key()`, `library_fold_text()`, the
  `library_collection_*` primitives and `collection_resolve()`.

- [ ] **Step 1: Implement the module**

In `collections.h` / `collections.c`:

- Name rules: non-empty, at most 200 bytes, not starting with `.`, none of
  `/ \ : * ? " < > |`. The GUI trims trailing spaces; the system validates.
- A lookup implementation for `collection_lookup_t` that calls the Task 3
  library APIs. The adapter copies `library_album_snapshot_t.title` to
  `collection_snapshot_t.album` and `.key` to `.album_key`. The `fold` callback
  is `library_fold_text`; it is pure.
- `collections_add_albums()`: one keyed value at a time; name-only values are
  skipped; snapshot; writer. Return counts.
- `collections_albums_page()`: raw page from `library_collection_page()`;
  resolve each row through `collection_resolve()`; fill `collection_album_t`;
  self-heal fallback steps through `library_collection_update_item()`; if a
  merge happened, re-read the same window once.
- `collections_track_paths()`: page 32 items; resolve; open
  `LIBRARY_LIST_TRACKS` + `LIBRARY_FILTER_ALBUM`; read in 256-row windows; append
  paths; cap 20,000; free everything on failure.
- `collections_remove_album()`, create/rename/delete/count/list/revision
  wrappers.
- Every failure logs `fprintf(stderr, "collections: ...")` and returns the
  documented empty answer; no silent failure and no crash on `db == NULL`.

- [ ] **Step 2: Build**

Run: `cd sonix-player/sonix-player && make host -j$(nproc) && make test-collections`
Expected: both pass.

- [ ] **Step 3: Commit**

```bash
git add sonix-player/src/system/library/collections.h sonix-player/src/system/library/collections.c
git commit -m "collections: expose the album collection API"
```

---

## Task 5: Collection detail page

**Files:**
- Create: `sonix-player/src/gui/library/collectionpage.h`
- Create: `sonix-player/src/gui/library/collectionpage.c`
- Modify: `sonix-player/src/gui/shell/gui.c` (create the screen and init)
- Modify: `sonix-player/src/gui/nowplaying/coverloader.h` (slots)

**Interfaces:**
- Produces: `extern lv_obj_t *collectionpage_screen;`
- Produces: `void collectionpage_init(gui_config_t *cfg);`
- Produces: `void collectionpage_open(int collection_id, const char *name);`
- Consumes: `collections_albums_page()`, `collections_album_count()`,
  `collections_remove_album()`, `collections_track_paths()`,
  `collections_paths_free()`, `collections_revision()`.

- [ ] **Step 1: Cover slots**

In `coverloader.h`, add:

```c
#define COVERLOADER_COLLECTION_BASE 108
#define COVERLOADER_COLLECTION_COUNT 10
#define COVERLOADER_SLOTS 118
```

Update the slot-map comment: 108..117 are the collection detail rows.

- [ ] **Step 2: Build the page shell**

- Create a screen with the back chevron.
- `settingsrow_title(screen, cfg, "collections")` then set the label text to the
  raw collection name (do not pass the name through `tr()`); reserve one corner
  slot for the play button with `settingsrow_title_corner_slots(title, cfg, 1)`.
- A viewport from `settingsrow_content_top(cfg)` to the bottom, a tall content
  object of height `count * 100`, and 10 fixed 100 px row widgets.
- Row: cover 72 px, title `font_ui_24` one line, artist `font_ui_16` dim, an
  ellipsis button.
- Scroll handler: `first = scroll_y / 100`, clamp to
  `count - visible_rows`, call `collections_albums_page(id, first, 10, rows)`,
  rebind the 10 widgets. Request a cover only when the row has a path (present,
  or a missing row whose stored `first_path` still exists); otherwise show
  `icon_album`.
- Empty state `collection_no_albums`.
- `screen_loaded_cb` reloads when `collections_revision()` changed; `open`
  resets scroll to 0 and stores the current id/name.

- [ ] **Step 3: Row behaviour**

- Tap a present row: `medialist_open(value, LIBRARY_LIST_TRACKS,
  LIBRARY_FILTER_ALBUM, value)`.
- Tap a missing row: `confirm_notice("collection_not_in_library",
  "collection_missing_note")` or a toast with the same key.
- Ellipsis: `collection_remove_album`; call `collections_remove_album(id, idx)`,
  toast `collection_removed`, reload. No confirmation.

- [ ] **Step 4: Play the collection**

- Corner play button opens a two-item popover:
  `medialist_play_in_sequence` and `medialist_play_in_random_order`.
- Both start a `collection_play_t` worker with `thread_be_low_priority("collectionplay")`:
  `int count = collections_track_paths(id, &paths);` then `gui_post()` the done
  callback with retries.
- Done callback:
  - in order: `playlist_set_mode(PLAYBACK_MODE_NORMAL)`;
  - shuffled: `PLAYBACK_MODE_SHUFFLE_REPEAT` when
    `musicsettings_endless_shuffle()`, else `PLAYBACK_MODE_SHUFFLE`;
  - `device_state_play_list(paths, count, 0)`,
    `player_refresh_now_playing()`, `switch_screen(player_screen)`.
  - free paths; `toast_busy_end()`; on zero paths show `collection_cannot_play`.
  - discard the result if the page generation changed.
- Show `toast_busy("collection_loading")` while the worker runs.

- [ ] **Step 5: Build**

Run: `cd sonix-player/sonix-player && make host -j$(nproc)`
Expected: clean build. The page is reachable through the list page (Task 6) and
the Music tile (Task 7); visual checks happen in Task 9. Do not claim a UI check
that the navigation cannot reach yet.

- [ ] **Step 6: Commit**

```bash
git add sonix-player/src/gui/library/collectionpage.h \
  sonix-player/src/gui/library/collectionpage.c \
  sonix-player/src/gui/shell/gui.c sonix-player/src/gui/nowplaying/coverloader.h
git commit -m "collections: show a collection's albums with covers and playback"
```

---

## Task 6: Collections list page and picker

**Files:**
- Create: `sonix-player/src/gui/library/collectionspage.h`
- Create: `sonix-player/src/gui/library/collectionspage.c`
- Modify: `sonix-player/src/gui/shell/gui.c` (create the screen and call
  `collectionspage_init(cfg)`)

**Interfaces:**
- Produces: `extern lv_obj_t *collectionspage_screen;`
- Produces: `void collectionspage_init(gui_config_t *cfg);`
- Produces: `void collectionspage_open(void);`
- Produces: `void collectionspage_add_album(const char *album_value);`
- Produces: `void collectionspage_add_albums(const char *const *album_values, int count);`
- Consumes: `collections_*` from Task 4; `tr()` keys from Task 7 (raw tags are
  acceptable until the language files land).

- [ ] **Step 1: Build the page shell**

Model it on `playlistpage.c`:

- `settingsrow_title(screen, cfg, "collections")`; no corner buttons, so
  `settingsrow_title_corner_slots(title, cfg, 0)`.
- A full-height vertical list (`LV_FLEX_FLOW_COLUMN`, 8 px gap, card rows 88 px).
- An empty label with `collection_empty_note`.
- A full-screen naming layer with an `lv_textarea` (max 200), a close button and
  `keyboard_create(..., "ok", name_accept_cb, NULL)`.
- `switcher_attach_back_gesture(screen)` and `player_sheet_attach_drag(screen)`.
- `screen_loaded_cb` rebuilds when `collections_revision()` changed.

- [ ] **Step 2: Build the rows**

- First row: `+ New collection` (`icon_plus`, accent, label
  `collection_new`).
- Each collection row: name (one line, ellipsis), subtitle
  `tr("collection_albums")` formatted with the count (singular
  `collection_album`), and an ellipsis button.
- In picker mode: no ellipsis, title `collection_add_to_collection`; tapping a
  row adds the pending albums.
- In browse mode: tapping a row calls `collectionpage_open(id, name)`.
- Row menu: Rename (naming layer prefilled) and Delete (`confirm_show` with
  `collection_delete_title` / `collection_delete_message`, then
  `collections_delete`, rebuild). Delete leaves the albums in the library.

- [ ] **Step 3: Naming flow**

- Trim trailing spaces; empty → `name_required`.
- `collections_exists` → `collection_name_taken`.
- Create: `collections_create`; error → `collection_cannot_create`.
- Rename: `collections_rename`; error → `collection_cannot_rename`.
- After create in picker mode, add the pending albums; after create in browse
  mode, rebuild.

- [ ] **Step 4: Picker mode and bulk add worker**

- `collectionspage_add_album` / `add_albums` deep-copy the values into a pending
  array, set `picking = true`, hide row menus, and `switch_screen`.
- On a row tap or after creating a collection: for one value, call
  `collections_add_albums()` synchronously and toast; for more than one, copy
  the pending values into a `collection_batch_t` (name, id, values, count,
  generation), show `toast_busy("collection_loading")`, and start a detached
  worker with `thread_be_low_priority("collectionadd")`.
- The worker calls `collections_add_albums()`; `gui_post()` the done callback
  (retry a few times if the queue is full, as the import worker does).
- Done callback: `toast_busy_end()`; choose `collection_added` (all added),
  `collection_some_not_added` (some added), or `collection_cannot_add`
  (nothing added; the counts do not distinguish duplicates from refused
  values, so do not claim a reason); free the batch; `back_btn_cb(NULL)` when
  returning to the caller.
- Free pending values on screen unload/next open.

- [ ] **Step 5: Build**

Run: `cd sonix-player/sonix-player && make host -j$(nproc)`
Expected: clean build. The page is reachable once Task 7 adds the Music tile;
visual checks happen in Task 9.

- [ ] **Step 6: Commit**

```bash
git add sonix-player/src/gui/library/collectionspage.h \
  sonix-player/src/gui/library/collectionspage.c sonix-player/src/gui/shell/gui.c
git commit -m "collections: add the collections page and picker"
```

---

## Task 7: Music tile, icon, translations and FEATURES

**Files:**
- Modify: `sonix-player/src/gui/library/music.h`
- Modify: `sonix-player/src/gui/library/music.c`
- Create: `sonix-player/assets/icons/collection.png`
- Modify: `sonix-player/tools/svg_to_lvgl.py`
- Regenerate: `sonix-player/src/gui/shell/icons.c`, `icons.h`
- Modify: 24 language files under
  `sonix-packer/assets/R1/usr/resource/sonix/language/` and
  `sonix-packer/assets/R3PII/usr/resource/sonix/language/`
- Modify: `FEATURES.md`

**Interfaces:**
- Produces: `MUSIC_TILE_COLLECTIONS` and a `TILES[]` entry calling
  `collectionspage_open()`.
- Produces: `icon_menu_collection` (or the album glyph as fallback).
- Produces: the `collection_*` language keys in all 24 files.

- [ ] **Step 1: Add the tile**

In `music.h`, add `MUSIC_TILE_COLLECTIONS` before `MUSIC_TILE_BROWSE`.
In `music.c`:
- `static void open_collections(void) { collectionspage_open(); }`
- a `TILES[]` row `{"collections", {"collections", &icon_menu_collection, NULL, open_collections}}`.

Existing saved tile orders append the unknown id at the end; no config migration.

- [ ] **Step 2: Add the icon**

Create a 140 px colour tile in the same family as the other Music section
icons (two gradient rounded squares with a translucent front, and a white
stack of album covers). Add `("collection.png", "menu_collection", SECTION_ICON_SIZE)`
to `tools/svg_to_lvgl.py` and run:

```bash
cd sonix-player/sonix-player
python3 tools/svg_to_lvgl.py
```

Review the regenerated `icons.c`/`icons.h`: only the new icon and the expected
symbol table churn. If the generator is unavailable, reuse `icon_menu_album` in
`music.c` and record the fallback in the commit message.

- [ ] **Step 3: Add the language keys**

Add a `; --- collections` block to all 12 files in both trees with:

`collections`, `collection_new`, `collection_name`, `collection_empty_note`,
`collection_no_albums`, `collection_albums`, `collection_album`,
`collection_add_to_collection`, `collection_added`,
`collection_some_not_added`, `collection_cannot_add`,
`collection_remove_album`, `collection_removed`,
`collection_not_in_library`, `collection_missing_note`,
`collection_delete_title`, `collection_delete_message`,
`collection_cannot_delete`, `collection_name_taken`,
`collection_cannot_create`, `collection_cannot_rename`, `collection_play`,
`collection_loading`, `collection_cannot_play`.

Use the existing generic keys (`name`, `name_required`, `ok`, `delete`,
`rename`, `remove`, `add_to_playlist`) where the wording is identical. Keep the
translations in the files' style; `collection_delete_message` takes the `%s`
name.

- [ ] **Step 4: Check every file has every key**

```bash
keys="collections collection_new collection_name collection_empty_note collection_no_albums collection_albums collection_album collection_add_to_collection collection_added collection_some_not_added collection_cannot_add collection_remove_album collection_removed collection_not_in_library collection_missing_note collection_delete_title collection_delete_message collection_cannot_delete collection_name_taken collection_cannot_create collection_cannot_rename collection_play collection_loading collection_cannot_play"
for f in sonix-packer/assets/*/usr/resource/sonix/language/*.ini; do
  for k in $keys; do grep -q "^$k = " "$f" || echo "missing $k in $f"; done
done
```

Expected: no output.

- [ ] **Step 5: Update FEATURES.md**

Under the Music library section add one bold-led bullet: collections of whole
albums, create/rename/delete, add from Albums or Now Playing, open an album,
play in order or shuffled, and missing albums stay visible and removable.

- [ ] **Step 6: Build**

Run: `cd sonix-player/sonix-player && make host -j$(nproc)`
Expected: clean build.

- [ ] **Step 7: Tile smoke test**

Run the simulator and open Music > Collections. Create, rename and delete a
collection; open the empty collection and confirm the detail page and its empty
state. Adding albums is wired in Task 8, so this step does not claim it.

- [ ] **Step 8: Commit**

```bash
git add sonix-player/src/gui/library/music.h sonix-player/src/gui/library/music.c \
  sonix-player/assets/icons/collection.png sonix-player/tools/svg_to_lvgl.py \
  sonix-player/src/gui/shell/icons.c sonix-player/src/gui/shell/icons.h \
  sonix-packer/assets/R1/usr/resource/sonix/language \
  sonix-packer/assets/R3PII/usr/resource/sonix/language FEATURES.md
git commit -m "collections: add the Music tile, icon and translations"
```

---

## Task 8: Add-to-collection entry points

**Files:**
- Modify: `sonix-player/src/gui/library/medialist.c`
- Modify: `sonix-player/src/gui/nowplaying/trackmenu.c`
- Test: host build + simulator flows

**Interfaces:**
- Consumes: `collectionspage_add_album()` and
  `collectionspage_add_albums()` from Task 6.
- Consumes: `collectionspage_screen` for `switcher_set_player_return()`.

- [ ] **Step 1: Albums selection menu**

In `medialist.c`:

- Split the current `sel_add_cb` into `sel_add_playlist_action` (the existing
  gather-and-open-picker code) and `sel_add_collection_action`.
- `sel_add_cb`: if `p->kind == LIBRARY_LIST_ALBUMS`, show a two-item popover on
  `p->sel_add_btn`:
  - `{"add_to_playlist", sel_add_playlist_action, p}`
  - `{"collection_add_to_collection", sel_add_collection_action, p}`
- Otherwise call `sel_add_playlist_action` directly; no behaviour change for
  tracks/artists/favourites/playlists.
- `sel_add_collection_action`: qsort `p->sel` by index; copy each
  `p->sel[i].key` (album values) into a heap array of `strdup`s;
  `selection_stop(p)`; call `collectionspage_add_albums(values, count)`; free the
  array. Do not call `sel_gather()`.
- `popover_show()` copies the item array, so the stack `items[2]` and
  `user = p` are safe.

- [ ] **Step 2: Now Playing menu**

In `trackmenu.c`:

- Keep `ITEMS` (5) and `ITEMS_PODCAST` (5).
- Add `ITEMS_LOCAL` with six items: queue, add to playlist, **add album to
  collection**, show album, details, settings.
- At `trackmenu_open`, choose:
  - podcast → `ITEMS_PODCAST`;
  - Qobuz/Tidal cached track (`qobuzcache_album_id` / `tidalcache_album_id`) →
    `ITEMS`;
  - local and `library_track_album_value(state.current_file, album, sizeof
    album)` → `ITEMS_LOCAL`;
  - otherwise → `ITEMS`.
- `add_album_to_collection_cb`: get state; get the album value; close the
  player sheet; `collectionspage_add_album(album)`;
  `switcher_set_player_return(collectionspage_screen)`.
- Keep the item as a static initializer so `tools/extract_strings.py` sees it.

- [ ] **Step 3: Build**

Run: `cd sonix-player/sonix-player && make host -j$(nproc)`
Expected: clean build.

- [ ] **Step 4: Simulator flow**

With a scanned fake card:
- Long-press an album in Albums → add button → "Add to collection" → pick a
  collection; confirm the album appears there.
- Select several albums → add to collection; confirm all appear in order.
- Play a track → three-dot → "Add album to collection"; confirm the album is
  added and the picker returns to the player.
- Repeat on a podcast and a Qobuz/Tidal track: the item is absent.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/gui/library/medialist.c sonix-player/src/gui/nowplaying/trackmenu.c
git commit -m "collections: add albums from the Albums list and Now Playing"
```

---

## Task 9: Integration, fixtures and release checks

**Files:**
- No production files unless a verification step finds a bug.
- Test: fixtures under `/tmp/opencode/albumcol-sd`, SQLite inspection, simulator.

- [ ] **Step 1: Confirm the host build dependencies**

Run: `pkg-config --modversion opusfile wavpack`
Expected: two versions. If not, see Execution Prerequisites.

- [ ] **Step 2: Generate tagged fixtures**

Use `ffmpeg` (present) to create FLAC albums under
`/tmp/opencode/albumcol-sd/Music`:

- two albums titled "Greatest Hits" with different `album_artist`;
- one album with no `album_artist` and no `artist`;
- one album with a 250-byte title;
- one album with Unicode title/artist;
- one two-disc album;
- one CUE sheet with a WAV;
- one album to delete later, and one to retag.

Example command shape:

```bash
ffmpeg -f lavfi -i "sine=frequency=440:duration=3" -metadata title="Track 1" \
  -metadata album="Greatest Hits" -metadata artist="Artist A" \
  -metadata album_artist="Artist A" -metadata track="1" \
  /tmp/opencode/albumcol-sd/Music/A/Greatest/01.flac
```

For multi-disc, add `-metadata disc="1"` / `"2"` and track numbers. For the CUE
fixture, write a `album.cue` beside a WAV with `FILE "album.wav" WAVE`,
`TRACK 01 AUDIO`, `TITLE "..."`, `PERFORMER "..."`, `INDEX 01 00:00:00`.
For the retag test, rewrite a copy with `ffmpeg -i in.flac -codec copy
-metadata album_artist="New Artist" out.flac` and move it over the original.

- [ ] **Step 3: Scan and exercise the feature**

Run the simulator with the fake card:

```bash
cd sonix-player/sonix-player
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
SONIX_SD_ROOT=/tmp/opencode/albumcol-sd \
SONIX_CONFIG=/tmp/opencode/albumcol-config.ini \
SONIX_LOG=/tmp/opencode/albumcol.log \
SONIX_NO_MOUNT=1 SONIX_NO_SYSSERVER=1 \
./sonix_player_host
```

Use an interactive SDL window for the UI steps; the dummy driver is only for
startup smoke tests. Run the flows in Review Focus 1–5:

- create/rename/delete; add one/many; add from Now Playing; open each album;
  remove; play in order and shuffled; leave/return; relaunch.
- long-name, Unicode, duplicate-name, multi-disc and CUE albums open correctly
  from the collection.
- delete an album's files, open the collection: it is missing, not gone; remove
  it.
- retag an album (case, accent, album artist) and run Detect changes; it follows
  or becomes missing, never a different album.
- enable `join_albums` with two items that collapse to one album; open the
  collection; the later item is merged and the page still works.
- pull/reinsert the card (or relaunch) and confirm the collection is intact.

- [ ] **Step 4: Database evidence**

```bash
DB=/tmp/opencode/albumcol-sd/.local/library.db
[ -f "$DB" ] || DB=/tmp/.local/library.db
sqlite3 "$DB" 'SELECT id,name FROM SONIX_COLLECTION;'
sqlite3 "$DB" 'SELECT collection,idx,album,album_key FROM SONIX_COLLECTION_ITEM ORDER BY collection,idx;'
sqlite3 "$DB" 'PRAGMA integrity_check;'
```

Expected: collections and items are present in insertion order; no duplicate
`(collection, album, album_key)`; `ok`.

- [ ] **Step 5: Rescan and rescue**

- Full rescan; collections and items survive; no stale-handle errors in the log.
- Rescue: stop the player, copy the DB aside, then corrupt one non-collection
  table so the collections stay readable:

```bash
DB=/tmp/opencode/albumcol-sd/.local/library.db
[ -f "$DB" ] || DB=/tmp/.local/library.db
cp "$DB" "$DB.good"
sqlite3 "$DB" "PRAGMA writable_schema=ON;
  UPDATE sqlite_master SET sql='CREATE TABLE MEDIA_TABLE(' WHERE name='MEDIA_TABLE';"
```

  Relaunch and run a scan. Expected: the log says
  `library: the damaged database is kept as ...`; the new DB still has the
  collections and items. If the damage path does not fire as expected, inspect
  `db_damaged`/`note_damage()` and record the finding rather than claiming the
  rescue passed.

- [ ] **Step 6: Builds and formatting**

```bash
cd sonix-player/sonix-player
make host -j$(nproc)
make test-collections
make target -j$(nproc)
clang-format --dry-run --Werror \
  src/system/library/collections_resolve.c src/system/library/collections_resolve.h \
  src/system/library/collections.c src/system/library/collections.h \
  src/gui/library/collectionspage.c src/gui/library/collectionspage.h \
  src/gui/library/collectionpage.c src/gui/library/collectionpage.h
git diff --check
```

Expected: host build, tests and target build pass; the new files are
`clang-format` clean; `git diff --check` reports no whitespace errors; no new
warnings. Do not reformat pre-existing code wholesale; format only the files
this feature creates. The PR checklist still requires manual testing on a
real R1 and R3 Pro II; record that in the PR, do not claim it from the simulator.

- [ ] **Step 7: Final review and commit**

Run the `requesting-code-review` skill on the branch. Fix every correctness
finding; re-run the build/tests. Commit any remaining fixes with a
`collections:` message.

---

## Execution Handoff

Tasks 1–6 are sequential: Tasks 5 and 6 both touch `gui.c` and the screen
lifecycle, so they must not run in parallel. After Task 6, Tasks 7 and 8 touch
disjoint files (Music/icons/languages/FEATURES vs medialist/trackmenu) and can
be dispatched in parallel with one implementer and one reviewer each. Task 9
runs once on the integrated branch.

The main working tree has unrelated uncommitted changes, including 24 language
files. Create an isolated worktree before Task 1 (`using-git-worktrees`), so
those changes are not mixed into this feature.
