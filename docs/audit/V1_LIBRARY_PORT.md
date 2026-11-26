# V1 library feature group — port audit

## Provenance

This group is ported from the TempoTec Variations V1 fork by **@mr-f0xx**,
specifically commit **335361a6c1939f7a4d1e1c8e8ab23e472c685c4a** ("Merge pull
request #408 from mr-f0xx/arena/67206235-sonix-player-tempotec-v1"). The
board-geometry values carried by the V1 fork were credited to @mr-f0xx in the
shared `board_profile.c` already present on this branch; that credit is kept
here for the imported code as well.

## Imported files

GUI (`src/gui/library/`):

* `albumgrid.c`, `albumgrid.h` — pure grid arithmetic, tested independently in the fork.
* `stats.c`, `stats.h` — listening-statistics page plus the two full "See all" pages.
* `geniusui.c`, `geniusui.h` — Smart Mix size picker, worker, progress overlay and playlist save.
* `pictureview.c`, `pictureview.h` — standalone JPEG/PNG viewer driven by `cover_load_image_file()`.

Supporting system code (`src/system/library/`):

* `genius.c/.h`, `geniusdb.c/.h`, `geniusbuild.c/.h` — graph reader, mix builder and snapshot database.
* `listeningstats.c/.h` — counting/flush logic and the Stats page's row types.
* `tagdate.c/.h`, `tagtext.c/.h`, `jisx0208.c/.h` — tag-date precision, encoding guess and JIS X 0208 table.

No other V1 group (games, streaming, settings, shell or audio) was imported, and
none of the imported screens is wired into the existing shell navigation on
this branch, so current HiBy behaviour is unchanged.

## Shared API changes

One backward-compatible field was added: `bool verbatim` at the end of
`popover_item_t` (`src/gui/shell/popover.h`), honoured in `popover.c`. Every
existing caller leaves it false and keeps getting `tr()` translation; only the
ported Smart Mix picker sets it for its already-formatted "N tracks" labels.

Everything else was adapted at the imported call sites rather than by widening
shared APIs:

* `bp_is_tempotec_v1()` → `bp_is_compact()`; `icon_stats_artists/albums` → the
  existing `icon_artist/icon_album`; `settingsrow_scale_corner_icon()` → the
  equivalent inline compact-only `lv_image_set_scale()`.
* `library_session()` and `playlists_create_from_seed()` → a local
  `v1_create_mix_playlist()` built on `playlists_exists/create/add_track/delete`;
  `playlists_delete_session()` → `playlists_delete()`; `playlistpage_open_named()`
  → `playlistpage_open()`.
* `library_get_indexed()` → a local safe default (paths treated as indexed).
* `library_read_tags()`/`library_tags_t` → a local declaration plus a safe
  default that reports an empty tag stream; the real walk belongs to the
  library-integration group. `library_revision()` is already shared.
* The V1 `LISTEN_*` table helpers are local no-op/empty defaults inside
  `listeningstats.c`; no card data is written and the page shows its real empty
  state. Wiring the counters into playback is a separate group.
* `sqlite3_progress_handler()` is removed: this tree builds SQLite with
  `SQLITE_OMIT_PROGRESS_CALLBACK`, and the reader already checks cancellation
  at each row and bulk read.
* Translation keys use this tree's English-source convention (e.g. "Listening
  time") instead of V1's `stats_*` keys, which have no entries in our `.ini`
  files. Existing keys such as `playlist_tracks` are reused unchanged.

## Screen adaptation

Geometry comes from `bp_is_compact()`, `bp_pick()` and the `gui_config_t`
handed to `stats_init()`/`pictureview_init()` — no `240x320` literal in layout
code. `albumgrid` takes its column count and tile gap/pad from the board
profile (2 columns on the V1, 4 on both HiBy panels) while measuring the actual
viewport it is given. The Stats page uses `bp_pick()` for card radii, chart
height, pill sizes and row/thumbnail sizes, and derives all margins from
`cfg->padding`; the picture viewer sizes its viewport from
`cfg->screen_width/height` and `settingsrow_content_top()`.

## Build

Target (from `sonix-player/`, build inputs copied from the
`integration-three-device` worktree):

```
make target -j8
EXIT=0
check-abi: no symbol beyond glibc 2.22.
Target build complete: File ready at 'sonix_player'
```

Host, with the supplied libraries:

```
PKG_CONFIG_PATH=/home/diogo/Downloads/Hiby/.host-libs/lib/pkgconfig \
LDFLAGS="-L/home/diogo/Downloads/Hiby/.host-libs/lib -Wl,-rpath,/home/diogo/Downloads/Hiby/.host-libs/lib" \
make host -j8
EXIT=0
```

## Visual verification (host SDL simulator, not hardware)

Per-device working directories carried each device's own `usr/resource/sonix`
and fonts; runs were sandboxed with bubblewrap, env
`SDL_VIDEODRIVER=dummy SONIX_PANEL=<WxH> SONIX_CONFIG=<ini> SONIX_SD_ROOT=/mnt/sd_0`,
and screenshots were taken through the application's own `/tmp/sonix/shot`
trigger. A temporary host-only preview hook switched to `stats_screen` for the
capture and was removed before this commit.

* `docs/QA/screenshots/r1/library.png` — **480×800**. Title, period pills, both
  summary cards, the seven-day chart frame and both Top cards fit with clear
  spacing; no clipping.
* `docs/QA/screenshots/r3proii/library.png` — **480×720**. Same composition on
  the shorter panel; the final "Top albums" card is partly below the fold, as
  expected on this scrolling page. No horizontal clipping.
* `docs/QA/screenshots/v1/library.png` — **240×320**. Compact typography and
  four pills on one line; summary cards remain side by side and the chart
  begins below them. The page continues under the bottom edge and scrolls.
  No overlapped or cut-off text in the visible area.

All three were opened and inspected; none was blank. Dimensions were confirmed
with PIL.

## Plugin candidate

Under the proposed extension architecture in
`docs/architecture/PLUGIN_ARCHITECTURE.md`, this group is a **plugin candidate**
rather than a core feature: it can ship as an optional library/statistics
extension with no changes to existing HiBy screens. Its design references are
the TempoTec V1 fork by **@mr-f0xx** and the modular Compás design by
**@Starnished66**.
