# V1 apps port (games, calculator, notes, pet)

## Provenance

The four UI app groups below come from the TempoTec Variations V1 fork by
**[@mr-f0xx](https://github.com/mr-f0xx)** — repository
`mr-f0xx/sonix-player-tempotec-v1`, commit `335361a6c1939f7a4d1e1c8e8ab23e472c685c4a`.
The V1 panel support already in `pr/three-device` is credited in
`docs/audit/PACKER_PROVENANCE.md`; this document covers only the apps.

## What was imported

| Directory | Files |
| --- | --- |
| `sonix-player/src/gui/games/` | `games.{c,h}`, `game2048.{c,h}`, `snake.{c,h}`, `dino.{c,h}` |
| `sonix-player/src/gui/calculator/` | `calculator.{c,h}`, `calcmodel.{c,h}` |
| `sonix-player/src/gui/notes/` | `notes.{c,h}`, `noteeditor.{c,h}`, plus `notescompat.{c,h}` (see below) |
| `sonix-player/src/gui/pet/` | `petpage.{c,h}`, `peticon.{c,h}`, `pikachu.{c,h}`, plus `petcompat.{c,h}` (see below) |

No other V1 feature group was imported. The two `*compat` files are small,
self-contained data layers written for this port so the Notes and Pet pages do
not need the V1 fork's `src/system/notes/` or `src/system/pet/` groups. Notes
are plain text files under `<card>/Notes`; the pet keeps its state in the
existing `[pet]` section of `device_config.ini` and credits audible playback
from the shared audio and device-state APIs. They implement only the surface
the imported pages call.

The apps are reachable from **More** and are initialised before `morepage_init()`
in `src/gui/shell/gui.c`. The grid scrolls now that it holds more than six
tiles. A host-only `SONIX_OPEN_V1_APP=<name>` hook in `src/main.c` opens an app
directly for the headless visual checks; it has no effect on the device build.

## Shared APIs added (backward compatible)

* `src/gui/board_profile.{c,h}` — `bp_title_font()`, `bp_artist_font()`,
  `bp_time_font()`, `bp_tile_label_font()`, `bp_queue_label_font()`,
  `bp_format_label_font()`. The regular defaults are the font objects the HiBy
  layouts already used; `fonts_init()` supplies compact rasters for the V1, so
  no existing HiBy screen changes.
* `src/gui/shell/theme.{c,h}` — `theme_semantic_t`, `theme_semantic_color()`
  and `theme_on_accent()`. Existing palettes and accent behaviour are unchanged.
* `src/gui/shell/settingsrow.{c,h}` — `settingsrow_corner_button_size()`,
  `settingsrow_place_corner_button()`, `settingsrow_scale_corner_icon()`,
  `settingsrow_header_text_left()`, `settingsrow_add_icon()` and
  `settingsrow_add_caption()`. They use `bp_pick()`/`bp_is_compact()`, so the
  HiBy values are the ones the shell has always used.
* `src/gui/shell/icons.{c,h}` plus `icons_v1apps.c` — four glyphs (`icon_notes`,
  `icon_pencil`, `icon_enter`, `icon_heart_filled`) and six menu tiles from the
  V1 fork's generated icon table, added without regenerating the HiBy icons.
* `sonix-packer/assets/{R1,R3PII}/usr/resource/sonix/language/English.ini` and
  `sonix-packer/v1/assets/V1/usr/resource/sonix/language/English.ini` — English
  strings for the apps' 78 translation tags, so the pages do not show raw keys.
  No existing entry was changed.

## Plugin candidate

In the proposed extension architecture (`docs/architecture/PLUGIN_ARCHITECTURE.md`
on the `plugin-architecture` branch), this group is intended as an **optional
plugin** rather than a core feature: it can be enabled or omitted without
changing the player's core. The references for that split are the V1 fork by
**[@mr-f0xx](https://github.com/mr-f0xx)** (`mr-f0xx/sonix-player-tempotec-v1`,
where these apps originated) and the modular design of **Compás** by
**[@Starnished66](https://github.com/Starnished66)**.

## Screen adaptation

Every page asks the shared board profile instead of carrying a 240×320 layout:
`bp_pick()` selects compact/regular sizes, `bp_is_compact()` switches the
games' stat layout and pixel lattice, `bp_padding()`, `bp_status_bar_h()` and
`bp_tile_radius()` come from the panel profile, and the new named font helpers
resolve to compact rasters on the V1. The conditional keyboard and corner
height expressions in the imported pages were rewritten as `bp_pick(...)`.
One binary therefore serves R1 (480×800), R3 Pro II (480×720) and V1
(240×320) without device-specific copies.

## Build

```
# from <worktree>/sonix-player, with the prebuilt toolchain and libs in place
make target
PKG_CONFIG_PATH=/home/diogo/Downloads/Hiby/.host-libs/lib/pkgconfig \
LDFLAGS="-L/home/diogo/Downloads/Hiby/.host-libs/lib -Wl,-rpath,/home/diogo/Downloads/Hiby/.host-libs/lib" \
make host
```

Result: both commands exited **0**. `make target` linked the shared binary,
passed the glibc-2.22 ABI check and produced `sonix_player` (11 MB stripped,
43 MB debug symbols). `make host` produced `sonix_player_host`.

## Visual verification

The host simulator was run under bubblewrap with SDL's dummy video/audio
drivers, the per-device `usr/resource/sonix` tree and a config with English as
the language. Each app was opened through the host-only hook, a screenshot was
requested after ~6 s and captured from the LVGL framebuffer. The largest PNG
was checked with PIL for exact panel dimensions and saved as:

| Device | Panel | Screenshot | Observation |
| --- | --- | --- | --- |
| HiBy R1 | 480×800 | `docs/QA/screenshots/r1/apps.png` | 2048 board, header and hint fully visible, no clipping |
| HiBy R3 Pro II | 480×720 | `docs/QA/screenshots/r3proii/apps.png` | Calculator display and all 20 keys fit the panel |
| TempoTec V1 | 240×320 | `docs/QA/screenshots/v1/apps.png` | Pet page: Pikachu art, habitat, meters and status all render within the compact panel |

All three images are real, non-blank renders at the exact panel size. A first
capture showed the first-boot Date and time page because the simulator config
had no clock setting; with `[clock] configured=true` the hooks reach the apps.
A later capture showed the calculator's main-menu tile because the screen
object had not been allocated; the screen is now created in `gui_init()` like
the other app screens. Both problems were fixed and the final captures above
were re-run. Screenshots are git-ignored and force-added.
