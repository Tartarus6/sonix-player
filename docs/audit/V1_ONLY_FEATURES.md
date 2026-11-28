# V1-fork features not ported into the unified tree

The V1 fork (`mr-f0xx/sonix-player-tempotec-v1` @ `e962720`) carries roughly
80 C source files with no counterpart in the local/upstream tree. Device support
(profile, compact UI, jack quirk, assets, packer) was ported in this work; these
features were **not**. They are listed here so the remaining gap is explicit and
so a future port can be planned rather than guessed.

The list was produced by comparing `find src -name '*.c'` between the fork
clone and this worktree (`comm -23`), then grouping by directory.

## Interface / applications

* `gui/calculator/` — calculator UI and model.
* `gui/games/` — games hub, 2048, Snake, Dino.
* `gui/library/albumgrid.c`, `geniusui.c`, `pictureview.c`, `stats.c`.
* `gui/metronome/` — metronome page and click.
* `gui/notes/` — notes editor and page.
* `gui/nowplaying/` — alternative layouts (AA, sticker, vinyl), play button,
  hi-res badge.
* `gui/pet/` — desktop pet page and icon.
* `gui/settings/` — battery profile, boot picture, ListenBrainz, OTA auto,
  plugins pages.
* `gui/shell/` — charge screen, gestures, main-menu icons, volume readout.
* `gui/streaming/` — Jellyfin, SoundCloud, Subsonic, podcast saved, Qobuz genre
  grid/order, network music, stock-keys UI.
* `gui/tapedeck/` — tape deck and sampler.
* `gui/bluetooth/btreceiversession.c`, `gui/wireless/listentogether.c`.

## System / support

* `system/addons/`, `system/core/region.c`, `system/listenbrainz/`.
* `system/audio/` — crossfade, tape recorder/effects/storage/WAV, USB host
  mode/probe, visualizer.
* `system/device/` — battery guard/history/profile, charge limit/mode/attached,
  boot picture, LED over I2C, release notes, evdev touch, USB export.
* `system/library/` — Genius/TagDate/TagText support, listening statistics,
  JIS X 0208 table.

## Why these were not ported

Each is a self-contained feature with its own assets, strings and settings; many
use `bp_*` helpers in ways that would need review on 480-pixel panels, and some
depend on V1-specific hardware (charge limit, I2C LED, USB host). Importing ~80
files plus their assets without device testing would risk breaking the working
R1/R3 Pro II build and would violate the "preserve working functionality"
principle. They remain available in the fork for a staged follow-up.

Some names overlap features already present in the unified tree under different
paths (for example DLNA and crossfade-related code); a per-feature comparison is
required before any import, not a blind file copy.
