# Album Collections — Design

**Date:** 2026-11-16
**Issue:** [Jepl4r/sonix-player#248 — [Feature] Album Collections](https://github.com/Jepl4r/sonix-player/issues/248)
**Status:** Approved approach (option 1: registry + snapshots, dedicated pages)
**Scope:** Sonix Player (R1 and R3 Pro II), local library albums only

---

## 1. Intent and why

The player already has playlists, but a playlist is a flat list of tracks. The
request is a shelf of **whole albums**: create a named collection, add albums
to it from the Albums list or from Now Playing, open a collection and see its
albums as albums, and play the collection in order.

The user's alternatives today are workarounds: a playlist holding the first
track of each album, or genre tags (176 genres, prepared in advance). The
feature is "Playlists for albums", not another tag browser.

---

## 2. Goals

- A new **Collections** tile in Music, beside Playlists.
- Create, rename and delete named collections.
- Add one album or many albums to a collection:
  - from the Albums list, by long-press selection;
  - from the Now Playing three-dot menu, for the playing track's album.
- Open a collection and see **albums**, not a flat track list.
- Open an album from the collection (the normal album page and its tracks).
- Remove an album from a collection.
- Play a whole collection in album order.
- Collections survive full rescans, Detect changes, reboots, and card removal.
- An album that leaves the library is shown honestly as missing, never silently
  dropped and never replaced by a different album with the same name.
- The rest of the player is not put at risk: no changes to the library list
  query engine, the scan, or the playlist implementation.

## 3. Non-goals

- No collections of tracks (playlists already do that).
- No Qobuz/Tidal/streaming albums. They have no library album value.
- No smart/automatic collections (genre, year, recently added).
- No backup/import file format in v1. The extension point is documented.
- No reordering of albums inside a collection in v1 (insertion order).
- No action on the album page itself; the Albums list selection and the Now
  Playing menu cover the requested flows.
- No SonixLink remote API in v1.

## 4. Anti-goals

The feature fails if any of these happen, even if the code "works":

- An album is silently attached to a different album because two albums share
  a name.
- A rescan deletes a collection or marks a changed album as missing when it is
  still there under a changed tag.
- A missing album disappears from the collection without the user asking.
- A corrupt database loses collections, while favourites and playlists survive.
- The player blocks or crashes while a scan, a card change, or a large
  collection is in progress.
- Existing Albums/Artists/Playlists screens regress.

---

## 5. Constraints from the codebase

- **Album identity is derived, not stored.** An album value is
  `<album name> + '\x1f' + <key>`, where the key is a hash of the album artist
  (or track artist), else of the folder. `ALBUM_GROUP_TABLE` is rebuilt on every
  full scan. No stable album id exists.
- **The index rebuilds on every full scan.** `MEDIA_TABLE`, `ALBUM_TABLE`,
  `ALBUM_GROUP_TABLE`, artist/genre/link/folder tables are emptied. User data
  must live in separate tables (the playlist model).
- **The database is per card:** `<card>/.local/library.db`. A read-only card
  falls back to `/tmp/.local/library.db`, which is session-only, like every
  other library feature.
- **One SQLite handle and one non-recursive `db_lock`.** Callbacks that run
  with the lock held must not call back into the library.
- **Only the GUI thread touches LVGL.** Workers use `thread_be_low_priority()`
  / `thread_be_background()` and `gui_post()`.
- **No test suite.** Verification is host build, target cross-build, simulator
  with a fake card, SQLite inspection, and the PR checklist (both devices
  manually).
- **Style:** C, tabs, `-Wall`, British spelling in comments, `tr()` for every
  user-visible string, all 12 languages in both asset trees.
- **The stock firmware may already own tables named `COLLECTION` or
  `collections`.** New tables use the `SONIX_` prefix.

---

## 6. Approaches considered

### Chosen — Registry + snapshots, dedicated pages

Two new tables in `library.db`, one system module, a Collections list page, a
Collection detail page, and small hooks in the Albums selection and Now Playing
menu. Resolution of the album reference happens in one place, with a cascade
and snapshot fallbacks.

Why: it isolates the feature from the 5,000-line medialist and the query
engine, keeps memory bounded with a windowed album list, represents missing
albums honestly, and cannot regress every other list in the player.

### Rejected — One table per collection + medialist integration

Mirrors playlists: `COL_<name>` tables, a new library list kind/filter, and the
existing Albums panel renders the collection. Less new UI, but requires deep
changes to `list_sql()`, `row_by_id_sql()`, row handles, generation domains,
and 256-byte value buffers; inherits the playlist table-name case collision;
and has no clean place for a missing album. Too much blast radius.

### Rejected — JSON/INI file on the card

No transactions, rewrites the whole file, the config store caps values at 128
bytes, and it sits outside the database lifecycle (locking, rescue, card
attach/detach). A second user-data mechanism for no benefit.

---

## 7. Data model

New tables, added to `SCHEMA[]` in `src/system/library/library.c`:

```sql
CREATE TABLE IF NOT EXISTS SONIX_COLLECTION(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL COLLATE NOCASE UNIQUE,
    created INT
);

CREATE TABLE IF NOT EXISTS SONIX_COLLECTION_ITEM(
    collection INT NOT NULL,
    idx INT NOT NULL,
    album TEXT NOT NULL COLLATE NOCASE,
    album_key TEXT NOT NULL,
    artist TEXT,
    folder TEXT,
    first_path TEXT,
    PRIMARY KEY(collection, idx),
    UNIQUE(collection, album, album_key)
);

CREATE INDEX IF NOT EXISTS sonix_collection_item_album_idx
    ON SONIX_COLLECTION_ITEM(album, album_key);
```

Notes:

- `id` is the stable collection identity. Names are data, never identifiers.
- `name` is `COLLATE NOCASE UNIQUE`; creation and rename also check explicitly
  so the user gets a proper "name taken" message. This fixes the playlist
  case-collision bug rather than copying it.
- `idx` is the insertion order and the stable row position used by the UI. It
  may have holes after removals; that is fine.
- `album` and `album_key` are stored separately, never as one 267-byte value.
  This avoids the 256-byte truncation in `library_index_t.value`,
  `medialist.filter_value`, and `medialist.menu_album`.
- `artist`, `folder` and `first_path` are **snapshots** taken when the album is
  added. They are fallbacks for resolution and for the missing-album row.
- `UNIQUE(collection, album, album_key)` prevents adding the same album twice,
  case-insensitively on the name.
- The `SONIX_` prefix protects against stock-firmware tables.
- Add both tables to `KEPT_TABLES` in `library.c` so a damaged-database rescue
  carries them over, exactly as favourites and playlists are carried.
- No scan hook is needed: nothing in the scan deletes these tables.

---

## 8. Identity and resolution

An album value is unstable across tag edits, `join_albums` toggles, and folder
moves. The design accepts that and resolves with a cascade. **It never guesses
when more than one album could match.**

For a stored item `(album, album_key, artist, folder, first_path)`, resolve in
this order:

1. **Exact value.** Build `album + '\x1f' + album_key` and ask for the album
   snapshot. This is the normal case and uses the album index.
2. **Same key and folded album name, unique.** The key is derived from the
   folded album artist (or the folder), so every album by one artist shares it;
   a key alone does not identify an album. If the album name alone changed case
   or accent, the key is still the same. Enumerate albums with that exact
   `album_key`; count only candidates whose album name folds to the stored name
   and which are corroborated by the stored artist, the stored folder, or (when
   both are empty) the stored title. Accept only when exactly one such candidate
   exists. A candidate with a different album name never qualifies, even when
   its key and artist match.
3. **First file path.** Ask `library_track_album_value(first_path)`. Accept
   only when the resolved album is corroborated: the resolved artist matches
   the stored artist, or the resolved folder matches the stored folder, or (when
   both snapshots are empty) the resolved title matches the stored name. This
   follows a renamed album or a changed album key when the file is still there.
4. **Name + artist.** Enumerate the album values whose name folds to the stored
   name; accept only if exactly one has an artist folding to the stored artist.
5. **Name + folder.** Among the same-name candidates, accept only if exactly
   one has a folder folding to the stored folder.
6. **Missing.** Show the snapshots, mark the row "Not in the library", offer
   removal. Do not write, do not attach.

There is **no "unique name" fallback**. A single album in the library carrying
the old name is not evidence that it is the same album; attaching to it would
violate the feature's central promise. If steps 2–5 do not identify the album,
it is missing.

**Comparison rules.** Album names, artists and folders are compared with the
library's own folding (`fold_text()`: case dropped, accented Latin reduced to
the base letter), because album keys are derived from folded text and a retag
that only changes accents must still resolve. Collection **names** are user
labels, not library metadata: they use SQLite `COLLATE NOCASE` (ASCII case
folding), so "Café" and "Cafe" may be two collections. This matches the
database's existing name collation and avoids a second derived name column.

When a fallback (steps 2–5) resolves **uniquely and with corroboration**, the
item's `album`, `album_key`, `artist`, `folder` and `first_path` are updated to
the resolved album so the next open takes step 1 again and a future same-named
album cannot steal the slot. Updates are batched in a savepoint.

**Collision during self-heal.** A retag or `join_albums` can make two items in
one collection resolve to the same album. The unique key
`(collection, album, album_key)` forbids two rows. The earlier position wins:
the later item is merged into the earlier one (deleted), the collection's album
count drops by one, the revision is bumped, and the page re-reads its window
once. The update primitive reports `UPDATED`, `MERGED` or `FAILED`; a failed
savepoint is rolled back and reported, never retried on every open.

The cascade lives in `src/system/library/collections.c`; the SQL primitives
live in `library.c`.

---

## 9. System API

### New library primitives (`library.h` / `library.c`)

```c
// A snapshot of one album, by album value. False when it is not in the index.
typedef struct {
    char title[256];
    char key[16];       // "a1b2c3d4" / "d1b2c3d4", without the separator
    char artist[256];   // album artist, else first track artist
    char folder[1024];  // folder of the first track
    char first_path[1024];
} library_album_snapshot_t;
bool library_album_snapshot(const char *album_value, library_album_snapshot_t *out);

// Every album whose name folds to `name`, as a full snapshot. Try the indexed
// NOCASE match first; scan with foldcase() only when that finds nothing,
// because the folded comparison has no index. The callback runs with the index
// locked: copy what you need, do not call back in.
typedef bool (*library_album_snapshot_cb)(const library_album_snapshot_t *snapshot, void *user);
int library_album_values_named(const char *name, library_album_snapshot_cb cb, void *user);

// Every album carrying this exact key, as a full snapshot. Normally one.
int library_album_values_with_key(const char *key, library_album_snapshot_cb cb, void *user);

// The library's folding, exposed so the resolver can compare a snapshot with a
// candidate the same way album_key() does. Truncates rather than overflows.
void library_fold_text(const char *in, char *out, size_t size);

// One item as stored. `idx` is the stable position.
typedef struct {
    int idx;
    char album[256];
    char album_key[16];
    char artist[256];
    char folder[1024];
    char first_path[1024];
} library_collection_item_t;

int  library_collection_create(const char *name);          // id, or -1
bool library_collection_exists(const char *name, int *id_out);
bool library_collection_rename(int id, const char *name);
bool library_collection_delete(int id);
typedef bool (*library_collection_cb)(int id, const char *name, int albums, void *user);
int  library_collection_list(library_collection_cb cb, void *user);
int  library_collection_count(int id);
int  library_collection_page(int id, int offset, int count, library_collection_item_t *out);
int  library_collection_remove(int id, const int *positions, int count);

// Adding many without holding them all in memory. The writer owns one
// savepoint; next index is worked out once. `write_row()` answers true when the
// row went in, false when it was a duplicate or the writer has failed;
// `write_end()` commits or rolls back and reports the counts and whether the
// transaction failed.
typedef struct library_collection_writer library_collection_writer_t;
library_collection_writer_t *library_collection_write_begin(int id);
bool library_collection_write_row(library_collection_writer_t *writer,
                                  const library_collection_item_t *item);
bool library_collection_write_end(library_collection_writer_t *writer, bool keep,
                                  int *added, int *skipped);

// Used only by the resolver's self-heal, never by the UI directly. Returns
// UPDATED, MERGED (a later item resolved to an album already held by an
// earlier position, so this row was deleted) or FAILED.
typedef enum {
    LIBRARY_COLLECTION_ITEM_FAILED = 0,
    LIBRARY_COLLECTION_ITEM_UPDATED,
    LIBRARY_COLLECTION_ITEM_MERGED,
} library_collection_update_result_t;
library_collection_update_result_t library_collection_update_item(
    int id, int position, const library_collection_item_t *item);

unsigned library_collections_revision(void);
```

Implementation notes:

- Add `GEN_COLLECTIONS` to `gen_domain_t`; bump it in every write;
  `library_collections_revision()` reads it under `db_lock`. Add it to
  `bump_all_generations()` so card changes invalidate open pages.
- The writer assigns `idx` from `MAX(idx)+1` and runs under one `SAVEPOINT`
  with `INSERT OR IGNORE`; it counts `sqlite3_changes()` for added/skipped, so
  peak memory is one item (about 2.6 KB), not the whole selection. A failed
  statement marks the writer failed; `write_end()` rolls back and reports it.
  This is the playlist writer pattern, applied to album rows.
- Remove takes the item `idx` values, in one savepoint.
- Delete removes items first, then the collection row, in one savepoint.
- All functions answer `false` / `-1` / `0` when `db == NULL` (card pulled,
  USB export), never crash.
- `library_collection_page` returns raw rows into a caller-owned buffer. The
  caller allocates it (a page is ~2.7 KB per row); nothing large goes on the
  stack. The lock is released before it returns, so the resolver can call the
  album APIs for each row.
- `library_collection_list` and the album-value callbacks run with the index
  locked; they must copy what they need and never call back into the library.

### Public module (`src/system/library/collections.c` / `.h`)

```c
typedef struct {
    int idx;
    char name[256];     // display title, current or snapshot
    char artist[256];
    char path[1024];    // cover source
    char value[LIBRARY_ALBUM_VALUE_MAX]; // current value, empty when missing
    bool missing;
} collection_album_t;

typedef bool (*collection_name_cb)(int id, const char *name, int albums, void *user);
int  collections_list(collection_name_cb cb, void *user);   // id, name, album count
bool collections_exists(const char *name, int *id_out);
int  collections_create(const char *name);
bool collections_rename(int id, const char *name);
bool collections_delete(int id);
int  collections_album_count(int id);
int  collections_albums_page(int id, int offset, int count, collection_album_t *out);
int  collections_add_albums(int id, const char *const *values, int count,
                            int *added, int *skipped);
bool collections_remove_album(int id, int position);
int  collections_track_paths(int id, char ***paths_out);    // malloc'd, capped
void collections_paths_free(char **paths, int count);
unsigned collections_revision(void);
```

Name rules: copy the playlist rules — non-empty, at most 200 bytes, does not
start with `.`, no `/ \ : * ? " < > |`. The GUI trims trailing spaces. The
system layer validates again.

`collections_add_albums()`:

- For each value, find the separator. A keyed value is taken as is. A name-only
  value is refused and counted as skipped: the UI only supplies keyed values,
  and adding by name alone could pick the wrong same-named album.
- Take `library_album_snapshot()`; if it fails, count as skipped.
- Build the item with the snapshot; `artist`/`folder`/`first_path` may be empty.
- Write it through `library_collection_write_begin()` /
  `write_row()` / `write_end()`. One item lives at a time; the whole selection
  is one transaction.
- Report added/skipped; the UI distinguishes "all added", "some added", and
  "nothing added". `skipped` includes both refused values and duplicates, so a
  caller must not infer the reason from the counts. Duplicates are skipped, not
  errors.

`collections_albums_page()`:

- Call `library_collection_page()` for raw rows (caller-owned array).
- Resolve each row through the cascade.
- Fill `collection_album_t`; on a fallback resolution, apply the self-heal
  through `library_collection_update_item()`. If any update returned `MERGED`,
  re-read the same window once before returning (the count shrank under it).
  A `FAILED` update is logged; the row is still shown resolved for this visit.
- A missing row uses the stored title/artist and `first_path` as cover source if
  the file still exists; otherwise it has no path.

`collections_track_paths()`:

- Page through items in batches; resolve each; for a resolved album open
  `LIBRARY_LIST_TRACKS` with `LIBRARY_FILTER_ALBUM` and `LIBRARY_ORDER_DEFAULT`,
  read it in 256-row windows, and append each path.
- Skip missing albums. Cap at 20,000 paths (the existing selection cap). Free
  with `collections_paths_free()`.

---

## 9.5 Supporting change: album value buffers

An album name is capped at 255 bytes (`metadata.h`), so an album value can be
`255 + 1 (separator) + 9 (key) + 1 = 266` bytes. Existing buffers are 256 in
several places, which truncates the key and turns a keyed value into a name-only
or broken one. That bug already affects "Show album" and the album head for very
long names; Collections must not inherit it.

Introduce `#define LIBRARY_ALBUM_VALUE_MAX 320` in `library.h` and use it for
every buffer that holds a whole album value:

- `medialist.c`: `winrow_t.name`, `panel_t.filter_value`, `panel_step_t.value`,
  `menu_album`, `np_album`, the row-click local, and the album-head local.
- `library.h`: `library_index_spec_t.value`.
- `library.c`: the list handle's `value`, and the `library_index_find_name()`
  local.
- `trackmenu.c`: the album buffer used by "Show album" / "Add album to
  collection".
- `playlist.c`: `queue_album` and `persisted_album` (the queue's album label).
- `device_state.c`: the album buffers used when saving/restoring the queue
  album.

This is a mechanical widening (320 instead of 256), not a change of behaviour.
It costs a few hundred bytes per open list. `coverflow.c`'s `cf_row_t.name[192]`
is left alone: it truncates for the carousel, a pre-existing limitation outside
this feature's promise, and the carousel does not open collections.

A host build with a 255-byte album name must show the album, open it, add it to
a collection, open it from the collection, and play it, with the key intact in
the database and in the log.

## 10. UI

### 10.1 Music tile

- Add `MUSIC_TILE_COLLECTIONS` before `MUSIC_TILE_BROWSE` in `music.h`.
- Add a `TILES[]` entry in `music.c`:
  - config id `"collections"` (never translated),
  - label `"collections"`,
  - action `open_collections()` → `collectionspage_open()`,
  - icon: a new `icon_menu_collection` (a colour tile at
    `assets/icons/collection.png`, matching the other Music section tiles: two
    gradient rounded squares with a white album-stack glyph; add it to
    `tools/svg_to_lvgl.py` and run the generator). If generation is not
    possible, fall back to `icon_menu_album`; do not block the feature on the
    icon.
- Existing saved tile orders do not mention the new id; `music.c` appends it to
  the end of the in-use list. Rearrange main page needs no change.

### 10.2 Collections list page (`src/gui/library/collectionspage.c/.h`)

Modelled on `playlistpage.c`, but simpler:

- Screen with `settingsrow_title(..., "collections")`, a vertical list, an
  empty label, and a full-screen naming layer with `keyboard_create()`. There
  are no corner buttons on this page, so call
  `settingsrow_title_corner_slots(title, cfg, 0)` to let the heading use the
  full width.
- Rows: a first row `+ New collection` (`icon_plus`), then every collection
  with its name and an "N albums" subtitle. Local row menu (ellipsis):
  - Rename → naming layer prefilled.
  - Delete → `confirm_show()` with the collection name; on confirm delete and
    rebuild. The message says the albums stay in the library.
- Naming layer: copy the playlist page's trim/empty/name-taken flow, with
  `collection_*` keys.
- Picker mode (see 10.3): same screen, title changes, no ellipsis, tapping a
  row adds the pending albums, and `back_btn_cb(NULL)` returns.
- `screen_loaded_cb` rebuilds if `collections_revision()` changed while the
  page was behind another screen.
- Attach the back gesture (`switcher_attach_back_gesture`) and the player-sheet
  drag, as the playlist page does.
- If the card is unavailable, show the empty note with a "no card" message and
  disable creation; all system calls already fail safely.

### 10.3 Collection picker

Public entry points:

```c
void collectionspage_open(void);
void collectionspage_add_album(const char *album_value);
void collectionspage_add_albums(const char *const *album_values, int count);
```

- `add_album` / `add_albums` deep-copy the pending values, set picker mode,
  switch to the page.
- In picker mode the title is `collection_add_to_collection`; the `+ New
  collection` row is first; tapping a collection adds the pending albums.
- New collection from the picker: name it, create it, then add the pending
  albums. If creation succeeds but the add fails, the empty collection stays
  and the user sees the error.
- After a successful add: success/partial/error toast and back. Free pending
  values on leave and on the next open.
- The page does not set `switcher_set_player_return`; the caller does (the Now
  Playing menu), exactly as the playlist picker does.

### 10.4 Collection detail page (`src/gui/library/collectionpage.c/.h`)

Public entry:

```c
void collectionpage_open(int collection_id, const char *name);
```

- Screen title: the collection name, with a corner **Play** button and the
  back chevron. `settingsrow_title()` runs its argument through `tr()`, so build
  the label with a fixed tag and then set its text to the raw collection name
  (`settingsrow_page_title()` finds the label, or keep the pointer returned by
  `settingsrow_title()`). Otherwise a collection named "delete" would show the
  translation of "delete". Reserve one corner slot for the play button.
- Album list: only visible rows exist. Fixed 100 px rows, a tall content area,
  and a scroll handler that rebinds the visible window from
  `collections_albums_page(id, first, pool)`.
  - 10 row widgets (cover 72 px, title `font_ui_24`, artist `font_ui_16`).
  - Cover slot base `108`, count `10`; bump `COVERLOADER_SLOTS` from 108 to 118
    and document the new range in `coverloader.h`. Missing rows request no
    cover and show `icon_album`.
  - Clamp the first index so the last page is full; handle a count of 0.
- Row tap: if present, `medialist_open(value, LIBRARY_LIST_TRACKS,
  LIBRARY_FILTER_ALBUM, value)`; if missing, a notice with
  `collection_missing_note`.
- Row ellipsis: "Remove from collection" → remove by `idx`, rebuild, toast.
  No confirmation: re-adding is one action.
- Empty state: `collection_no_albums`.
- **Play the collection:** the corner play button opens a two-item popover
  using the existing `medialist_play_in_sequence` and
  `medialist_play_in_random_order` keys. Both start a low-priority worker that
  calls `collections_track_paths()`, then `gui_post()` to the GUI thread. The
  GUI callback:
  - in order: `playlist_set_mode(PLAYBACK_MODE_NORMAL)`, then
    `device_state_play_list(paths, count, 0)`;
  - shuffled: use `PLAYBACK_MODE_SHUFFLE_REPEAT` when
    `musicsettings_endless_shuffle()` is on, else `PLAYBACK_MODE_SHUFFLE` (the
    same policy as `play_artist()`).
  Then refresh Now Playing, switch to the player, and free the paths. A busy
  toast is shown; a generation counter discards the result if the page was
  left. If `gui_post()` is full, retry a few times as the import worker does,
  then free and report an error.
- `screen_loaded_cb` reloads when the revision changed (album removed from
  another page, card changed).
- On open, reset scroll to the top and set the current id/name. The page keeps
  no pointer to a previous collection's rows.

### 10.5 Albums list selection — "Add to collection"

`medialist.c`:

- `sel_add_cb` currently gathers tracks and opens the playlist picker. Change
  it so that, on an albums list (`p->kind == LIBRARY_LIST_ALBUMS`), it opens a
  two-item popover anchored on the add button:
  - `add_to_playlist` → the existing gather-and-pick flow;
  - `collection_add_to_collection` → read the selected album values directly
    from `p->sel[i].key` (they are album values, by `row_key()`), in list order,
    copy them, `selection_stop(p)`, then `collectionspage_add_albums()`.
- Do **not** use `sel_gather()` for the collection action: it expands albums to
  their tracks, which is the opposite of the feature.
- Other lists keep the current direct behaviour.
- `popover_show()` copies the item array, so stack items with `user = p` are
  safe; the panel outlives the popover.
- The selection bar keeps its four buttons; no layout change and no fifth
  button.

### 10.6 Now Playing three-dot menu — "Add album to collection"

`trackmenu.c`:

- Add a local-album table with six items; the existing five-item tables stay for
  podcasts and streaming.
- At menu open, choose the table:
  - podcast → `ITEMS_PODCAST`;
  - Qobuz/Tidal cached track → the five-item table (no album action);
  - local track with `library_track_album_value()` true → the six-item table;
  - otherwise → the five-item table.
- The action: `device_state_get()`, get the album value, close the player
  sheet, `collectionspage_add_album(value)`, and
  `switcher_set_player_return(collectionspage_screen)`.
- The item uses a static initializer so the string extractor sees it.

---

## 11. Missing albums

- They stay in the collection. No automatic pruning.
- The detail row shows the stored title and artist, dimmed, with
  `collection_not_in_library`, and no cover unless `first_path` still exists.
- Tapping shows `collection_missing_note`; the ellipsis still offers removal.
- A full rescan, a card swap, or a retag never deletes an item.
- The collection list subtitle counts all items, including missing ones.

---

## 12. Threading, errors and card lifecycle

- System functions take `db_lock` internally; they are safe from workers.
- GUI-thread work: list/row drawing, dialogs, all LVGL.
- Workers: adding many albums (picker), playing a collection. Both use
  `thread_be_low_priority()`, `gui_post()`, a busy toast, and a generation
  counter to discard stale results.
- A single album add from Now Playing is synchronous; it is one indexed lookup
  and one insert.
- Card removed / USB export: `db == NULL`; every call returns empty/false; the
  page shows its empty state. No cached SQLite handles outlive the card.
- Database error: log with `fprintf(stderr, "collections: ...")`, show the
  matching `collection_cannot_*` popup; never fail silently.
- Every write is in a savepoint; a failed multi-add leaves no partial rows.

---

## 13. Strings and documentation

New language keys (names indicative; keep the `collection_` prefix):

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

Reuse `name`, `name_required`, `ok`, `delete`, `rename`, `remove`,
`add_to_playlist`.

Add a `; --- collections` block to all 12 language files in both
`sonix-packer/assets/R1/.../language/` and `.../R3PII/.../language/`.
`tr()` returns the tag when a key is missing, so a missed file is visible.

Update `FEATURES.md` under the Music library section with a short user-facing
bullet for Collections, including that missing albums stay visible and can be
removed. Do not change `PATCHES.md` (that is for stock-firmware patches). No
README change unless a new directory or build step is added (none is).

---

## 14. Verification

No automated tests exist. The implementation is verified by evidence, not
assertions:

1. **Host build:** install the missing dev packages (`opusfile-devel`,
   `wavpack-devel` on Fedora; `libopusfile-dev`, `libwavpack-dev` on Debian),
   then `make host -j$(nproc)`. No new warnings.
2. **Target cross-build:** `make target -j$(nproc)`; the ABI check must pass.
3. **Formatting:** `clang-format --dry-run --Werror` on every touched C/H file.
4. **Fake card:** generate tagged albums with `ffmpeg` (present on this
   machine) under `SONIX_SD_ROOT`, including:
   - two albums with the same name and different album artists;
   - an album with no album artist (folder-based key);
   - a very long album name (near 255 bytes);
   - Unicode names;
   - a multi-disc album;
   - a CUE sheet album.
5. **Simulator flows:** scan; create/rename/delete a collection; add one and
   many albums from the Albums list; add from Now Playing; open an album from
   the collection; remove an album; play the collection in order and shuffled;
   leave and return to the page; relaunch.
6. **Rescan and retag:** add albums, rescan, confirm the collection is intact;
   change an album's case, then an accent, then the album artist, then enable
   `join_albums`, and confirm the cascade follows the album or marks it missing
   when ambiguous; delete an album's files and confirm the row becomes missing
   and can be removed; create a case where two items resolve to one album and
   confirm the later is merged, not duplicated or errored.
7. **Database checks:** `sqlite3` on the library DB confirms the two tables,
   item order, snapshots, and no duplicate `(collection, album, album_key)`.
8. **Corruption rescue:** copy the DB, corrupt/rename it as the existing rescue
   path expects, reopen, and confirm collections are carried across (both
   tables are in `KEPT_TABLES`).
9. **Memory/robustness:** build a host binary with AddressSanitizer if the
   Makefile allows a flag override, run the flows, and check the log for leaks,
   `gui: message queue full`, SQLite errors, and stale handles.
10. **Manual devices:** the PR checklist still requires the R1 and R3 Pro II.
    This environment can only cross-build and simulate; hardware verification
    must be called out explicitly in the PR.

---

## 15. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Album value changes after retag/join/move | Snapshot + corroborated cascade; self-heal only when unique |
| Two albums share a name | Key stored separately; never attach on name alone; no unique-name fallback; ambiguous → missing |
| Long album name | Name and key in separate columns; supporting buffer widening to `LIBRARY_ALBUM_VALUE_MAX` |
| Huge multi-album selection | Writer API: one item in memory at a time, one savepoint |
| Self-heal resolves two items to one album | Earlier position wins; later item merged, revision bumped, window re-read |
| Duplicate collection names differing by case | `COLLATE NOCASE UNIQUE` + explicit check |
| DB corruption | Both tables in `KEPT_TABLES` |
| Scan concurrent with add/remove | `db_lock` + savepoints; failure reported, no partial rows |
| Large collection | Windowed detail list; paged DB reads; play worker with a 20k cap |
| Card removed mid-page | `db == NULL` paths; revision bump on card change |
| UI regressions | Only medialist's `sel_add_cb` changes; no query-engine or scan changes |
| Translation misses | All 24 INI files; `tr()` shows raw tags when missing |
| Blocking the UI | Workers for bulk add and play; single add is one indexed insert |

---

## 16. Left to the builder

- Exact widget positions, paddings, row height inside the 100 px budget, and
  the empty-state wording/style.
- Whether the detail page uses one tall content object with absolute rows or
  recycled widgets moved into place; the required behaviour is bounded rows,
  correct rebinding on scroll, and no cover request for missing rows.
- Which existing play/album glyphs to use, and whether the new collection SVG
  is drawn or the album glyph is reused.
- Worker thresholds (when a bulk add is large enough to leave the GUI thread).
- The exact savepoint granularity for self-heal updates (one per page is
  expected).
- Test fixture details and the fake-card layout.
- Exact translations, subject to review.

---

## 17. Decisions already made

- **Option 1** (registry + snapshots + dedicated pages), chosen by the human
  partner after reviewing the three approaches.
- **No backup/import** in v1; the card-scoped database and `KEPT_TABLES` rescue
  are the durability story. The schema leaves room for a later file format.
- **No reordering** in v1; insertion order.
- **Play the collection** is included.
- **Local albums only.**
- **No album-page action**; the Albums list selection and Now Playing cover it.
- **Selection uses a popover** on the existing add button, not a fifth corner
  button, because the 480 px title would not survive five 56 px buttons.
- **No unique-name fallback**; when steps 2–5 cannot identify the album, it is
  missing. A young album with the old name is never substituted.
- **Play opens a two-item menu** (in order / shuffled) rather than silently
  overriding the user's shuffle mode.
- **Bulk add uses a streaming writer**; memory is one item, the transaction is
  the whole selection.
- **Self-heal merges duplicates**; the earlier position wins.
- **Album matching folds accents; collection-name uniqueness is NOCASE.**
- **Album value buffers are widened to 320 bytes** as a supporting fix; without
  it, a 255-byte album name loses its key before the collection ever sees it.
