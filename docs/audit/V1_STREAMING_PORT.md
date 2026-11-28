# V1 streaming feature group — port audit

## Provenance and credit

All imported code comes from the TempoTec V1 fork by **@mr-f0xx**,
`.v1-source/sonix-player` at commit `335361a6c1939f7a4d1e1c8e8ab23e472c685c4a`
("Merge pull request #408 … tempotec-v1"). Only the streaming group was
imported; the games, library, settings, shell and audio groups were left out.
The modular packaging idea follows **@Starnished66**'s Compás design.

## Imported files

`src/gui/streaming/`: `jellyfin_icon.{c,h}`, `jellyfinpage.{c,h}`,
`soundcloud_icon.{c,h}`, `soundcloudpage.{c,h}`, `subsonicpage.{c,h}`,
`subsonicdlpage.{c,h}`, `qobuzgenregrid.{c,h}`, `qobuzgenre_icons.{c,h}`,
`qobuzgenreorder.{c,h}`, `podcastsaved.{c,h}`, `networkmusicpage.{c,h}`.

`src/system/streaming/`: `jellyfin.{c,h}`, `jellyfincache.{c,h}`.

Supporting V1 headers kept for those call sites (no V1 backend `.c` files were
imported): `soundcloud.h`, `soundcloudcache.h`, `subsonic.h`, `subsonicdl.h`,
`subsonicstream.h`, `streambridge.h`, `networkmusic.h`, `podcastplayed.h`,
`streamkeep.h`, plus `src/system/net/networkshare.h` and
`src/system/library/tagdate.h`.

## API changes (all additive / backward compatible)

* `gui/shell/theme.{h,c}` — `theme_semantic_t`, `theme_semantic_color()` and
  `theme_uses_function_glyphs()`. On HiBy panels every semantic role resolves
  to the active accent (MUTED to secondary text) and function-glyph mode is
  off, so existing screens are unchanged.
* `gui/shell/gridpage.{h,c}` — tile restyling helpers
  (`set_tile_icon_style`, `set_tile_icon_size`, `set_tile_orientation`,
  `set_tile_caption`, `set_tile_label_lines`) used by the ported grids.
* `gui/shell/settingsrow.{h,c}` — leading icon/caption and corner-button
  helpers, drawn with the existing card and icon styles.
* `gui/streaming/streaming.{h,c}` — `STREAMING_LAN_PREVIEW_ONLY 0`, a no-op
  preview-action helper, and a gated preview hook (`SONIX_V1_STREAMING=1`,
  optional `SONIX_V1_STREAMING_OPEN=<page>`) used only for visual verification.
  The stock four-tile Streaming menu is unchanged by default.
* `gui/streaming/podcastpage.{h,c}` — shared `podcastpage_episodes_newest_first`
  getter/setter, persisted under the V1 config key.
* `system/net/tls.{h,c}` — `tls_verify_t`, `tls_verify_mode()` (always STRICT)
  and `tls_set_verify_mode()` (deliberately does not weaken verification).
* `system/net/http.{h,c}` — minimal `http_stream_open_ex()` with an options
  struct (extra headers, socket observer, status output). With no options it
  is `http_stream_open()`. Authentication headers go on the first hop only;
  HTTPS-to-HTTP downgrade stays disabled.
* New `v1_streaming_compat.{h,c}` and `qobuzgenre_compat.{h,c}` provide
  no-network, no-card defaults for the V1-only service APIs. They report
  "not configured"/empty results and never touch sockets or storage. The
  date parser needed by the saved-podcast list is implemented locally.

## Adaptation approach

* All `bp_is_tempotec_v1()` call sites were rewritten to the shared
  `bp_is_compact()`. Layouts use `bp_pick`, `bp_padding`, `bp_tile_radius`,
  `bp_status_bar_h` and `cfg->screen_width/height`; no 240×320 constants were
  carried over. The Jellyfin home grid shrinks its glyphs and captions on the
  V1 panel so no tile content spills outside its card.
* V1-specific missing icons are aliased to existing glyphs in
  `gui/streaming/v1_streaming_icons.h`.
* `json_parse_limited`/`json_str_exact` map to the unified `json_parse`/
  `json_str`; `content_length64` to `content_length`; V1 chunk flags and gzip
  requests were dropped. The old writable-path playback hook is bridged to the
  read-only unified hook.
* The saved-podcast page builds its date from the unified metadata reader's
  year field, falling back to the file mtime for sorting.
* Services with no backend in this tree (SoundCloud, Subsonic, network folders,
  keep-track, heard-flag, Qobuz genres) show their empty/not-configured states.
  Jellyfin uses its real imported backend with the shared HTTP/TLS helpers.

## Build result

* `make target -j4` — **exit 0** (`check-abi` clean, firmware binary and
  `sonix_launch` produced).
* `PKG_CONFIG_PATH=/home/diogo/Downloads/Hiby/.host-libs/lib/pkgconfig
  LDFLAGS="-L…/.host-libs/lib -Wl,-rpath,…/.host-libs/lib" make host -j4`
  — **exit 0**.

## Visual verification (host SDL simulator, not hardware)

Captured with the app's own `/tmp/sonix/shot` path under bubblewrap, using each
device's `system-info.json`, language files and `SONIX_PANEL`:

| Device | File | Size | Observation |
|---|---|---|---|
| HiBy R1 | `docs/QA/screenshots/r1/streaming.png` | 480×800 | Six-tile Jellyfin home grid, search button, readable captions. |
| HiBy R3 Pro II | `docs/QA/screenshots/r3proii/streaming.png` | 480×720 | Same composition with the shorter panel; no overlap. |
| TempoTec V1 | `docs/QA/screenshots/v1/streaming.png` | 240×320 | Compact profile: 6 px padding, 24 px status bar, translated single-line captions and fitted glyphs. |

All three PNGs were opened and inspected after capture; dimensions were
verified with PIL. This is simulator evidence only — no physical-device claim.

## Plugin candidacy

Per the proposed extension architecture in `docs/architecture/PLUGIN_ARCHITECTURE.md`,
this group is a **plugin candidate**, not a core feature: the pages are built
only under the `SONIX_V1_STREAMING` preview switch and otherwise leave the
shipped Streaming menu untouched, so a future plugin loader can adopt them
without disturbing the HiBy devices. The V1 fork by **@mr-f0xx** and the
modular Compás design by **@Starnished66** are the references for that
packaging step.

## Known limitations

* Stubbed services are UI-only until their backends are ported or replaced by
  plugins.
* Simulator language files predate the fork, so R1/R3 captures show untranslated
  V1 tags; the V1 overlay supplies translations.
* The read-only playback hook cannot carry a replacement stream path back to
  the player; harmless while the affected caches are stubs.
