# V1 shell port audit

## Provenance

The files below come from the TempoTec V1 fork by **@mr-f0xx**, commit
`335361a`. Source copy: `.v1-source/sonix-player`. Each ported file carries the
same attribution comment. No games/library/streaming/settings/audio files were
imported.

Scope: V1-only shell/theme files only. The existing shared
`gui/shell/theme.c` and `theme.h` were left untouched.

## Files added

| File | What it provides |
|---|---|
| `sonix-player/src/gui/shell/gestures.c`, `.h` | Persisted choices for classic/stock sheet edges and swipe-up-home. |
| `sonix-player/src/gui/shell/volumereadout.c`, `.h` | Nominal output voltage in place of the 0..100 index, with integer-only arithmetic and three-significant-figure formatting. |
| `sonix-player/src/gui/shell/main_menu_icons.c`, `.h` | Pre-rasterised A8 glyphs and ARGB illustrations for the six home tiles; generated data, kept byte-for-byte apart from attribution. |
| `sonix-player/src/gui/shell/chargescreen.c`, `.h` | LVGL charging screen with gauge, cable/key loop, panel wake bracket and timeout. |

## Shared API additions

None. No new shared header or library was added, and `theme.c`/`theme.h` were
not replaced. The only edit to an existing source is one call site in
`sonix-player/src/gui/shell/topbar.c`: the status-bar volume label now prints
`volumereadout_text()` instead of a raw `%d`, so the ported readout is reachable
from the UI. With the option off (the default) the text is unchanged.

## Adaptations to the unified tree

* **`chargescreen.c`** — the fork's V1-only dependencies were replaced with
  existing shared APIs: `backlight.h`'s safe-level helper is repeated locally;
  `battery_read_now()` became `sync_battery_from_sysfs()` +
  `read_battery_percent()` / `read_battery_charging()` (honouring the config's
  override paths); `charger_attached()` became a local supply scan matching the
  tree's `system/device/chargescreen.c`; `webemu.h` calls were dropped (this tree
  has no web emulator); `THEME_KIND_*` was replaced by the two classic palettes
  this tree actually ships; the four missing translation tags fall back to
  readable English. The header guard was renamed to avoid colliding with the
  existing `system/device/chargescreen.h`.
* **Layout** — all geometry uses `bp_pick()`, `bp_is_compact()`, `bp_padding()`,
  `bp_status_bar_h()` and the panel size `main.c` passes in `gui_config_t`:
  compact (240x320) gets a 72%-wide battery, 6 px padding, 24 px status band,
  smaller radii and the 16/18/28 font rungs; both 480-wide panels (480x800,
  480x720) get the 55% battery, 15 px padding, 44 px band and larger rungs.
  No hardcoded 240x320 assumptions remain.
* **`volumereadout.c`** — the fork's `alsa_volume_output_db100()` is
  `alsa_volume_gain_db100()` here; every other shared call exists unchanged.
* **`gestures.c`** — uses the existing `config_get_bool()` / `config_set_bool()`
  API directly; no V1-only dependency.
* **`main_menu_icons.c`** — self-contained generated LVGL image descriptors;
  it compiles against the unified tree's LVGL 9.6 without change.

## Build

* Host: `PKG_CONFIG_PATH=/home/diogo/Downloads/Hiby/.host-libs/lib/pkgconfig
  LDFLAGS="-L/home/diogo/Downloads/Hiby/.host-libs/lib
  -Wl,-rpath,/home/diogo/Downloads/Hiby/.host-libs/lib" make -C sonix-player host`
  — **exit 0**.
* Target: `make -C sonix-player target` — **exit 0**
  (`sonix_player` + `sonix_launch` produced). No warnings from the ported files;
  the warnings in the log are pre-existing (sqlite3 amalgamation, radioflac).

## Simulator verification

Host SDL simulator, bubblewrap-wrapped, with each device's own
`system-info.json` and fonts, `SONIX_PANEL` set per panel and a config that
enables the ported voltage readout. Frames were taken through the application's
own `/tmp/sonix/shot` screenshot path, the largest PNG per device opened and
inspected, and its dimensions verified with PIL.

| Device | Panel | Screenshot | Result |
|---|---|---|---|
| HiBy R1 | 480x800 | `docs/QA/screenshots/r1/shell.png` | PASS — six-tile grid, status bar, "25.2mV" readout with no overlap. |
| HiBy R3 Pro II | 480x720 | `docs/QA/screenshots/r3proii/shell.png` | PASS — same layout at the shorter panel. |
| TempoTec V1 | 240x320 | `docs/QA/screenshots/v1/shell.png` | PASS after fix — compact two-column grid; the first capture showed the 25.2mV label colliding with the centred clock, so compact voltage mode now uses the 16 px font rung and a tighter group. Re-captured: clear separation. |

The charging screen itself has no host trigger (the existing boot-time charge
decision is a separate V1-only file and was deliberately not imported); its
layout was adapted and compiled, but not exercised in these frames. Screenshots
are simulator evidence, not device evidence. Gestures are a persisted config
API with no distinct visual state on this screen.

## Plugin candidacy

Per the maintainer's updated strategy, this group is a **plugin candidate**
under the proposed extension architecture in
`docs/architecture/PLUGIN_ARCHITECTURE.md`: it is self-contained, attributed to
the TempoTec V1 fork by **@mr-f0xx** (commit 335361a), and follows the modular
design direction credited to **@Starnished66** (Compás). It is committed as an
optional, removable group — no PR is opened and nothing is pushed.
