# V1 settings / system extras port — audit

Source: TempoTec V1 fork by **@mr-f0xx**, commit `335361a`
(`Merge pull request #408 from mr-f0xx/arena/67206235-sonix-player-tempotec-v1`).
All code below is credited to that fork; this port only adapts it to the
unified three-device tree. No games, library, streaming, shell or audio feature
groups from the fork were imported.

## Files added

- `sonix-player/src/gui/settings/`: `batteryprofilepage.[ch]`, `bootpicpage.[ch]`,
  `listenbrainzsettings.[ch]`, `ota_auto.[ch]`, `plugins.[ch]`.
- `sonix-player/src/system/addons/`: `addons.[ch]`.
- `sonix-player/src/system/device/`: `batteryguard.[ch]`, `batteryhistory.[ch]`,
  `batteryprofile.[ch]`, `bootpic.[ch]`, `chargelimit.[ch]`, `chargemode.[ch]`,
  `chargerattached.[ch]`, `releasenotes.[ch]`.
- `sonix-player/src/system/listenbrainz/`: `listenbrainz.[ch]`.

Host-only QA hooks were added to `main.c` (`SONIX_OPEN_SETTINGS`,
`SONIX_OPEN_SYSTEM_SETTINGS`, `SONIX_OPEN_PAGE`, `SONIX_SCROLL_Y`); they do
nothing on the device build.

## Model gating and safety

Every new page and worker is behind `bp_is_compact()` (the TempoTec V1 model
flag) at its call site: GUI page construction in `gui.c`, the rows in
`appearance.c`, `musicsettings.c` and `systempage.c`, and the init/worker
calls in `main.c`, `wifi.c`, `fwupdate.c`, `system.c` and `power.c`.
`bp_pick()` / `bp_is_compact()` and `cfg` dimensions are used for the ported
UI's sizes and fonts; no HiBy layout values were changed.

**Power safety:** the PMIC/charging helpers are either gated or unwired. The
only new call that touches charging state indirectly is
`batteryhistory_tick()` from the existing battery poll, V1-only, which writes
settings (charge marks and on-time) and never a PMIC register. The V1-only
shutdown path calls `batteryhistory_flush()` while settings are still
writable. `batteryguard`, `chargelimit`, `chargerattached` and `chargemode`
are ported as pure helpers but have **no call sites**: the unified tree
already has `lowbattery.c`, `chargescreen.c` and the MP2731/AXP charge limit,
so wiring them would duplicate power behaviour. R1/R3 power behaviour is
unchanged. `http_req_t.no_https_downgrade` is an additive, opt-in flag
(default false) that keeps the ListenBrainz token from being re-sent over
plain HTTP after an HTTPS redirect; no existing HTTP caller changes.

## Build

- `make target -j4` — exit 0 (`Target build complete`, glibc 2.22 ABI check
  passed).
- `PKG_CONFIG_PATH=/home/diogo/Downloads/Hiby/.host-libs/lib/pkgconfig
  LDFLAGS="-L.../.host-libs/lib -Wl,-rpath,.../.host-libs/lib" make host -j4`
  — exit 0.

## Simulator QA (host build, bubblewrap, dummy SDL)

- `docs/QA/screenshots/r1/settings.png` — 480x800. Settings page renders with
  the original rows only; no V1 entries leak onto the R1.
- `docs/QA/screenshots/r3proii/settings.png` — 480x720. Same: the V1 rows are
  absent.
- `docs/QA/screenshots/v1/settings.png` — 240x320. Compact layout, title and
  rows fit the panel. Additional inspected V1 views: System shows **Battery
  profiler**, **Plugins** and a working **Update notifications** switch;
  Appearance shows **Boot picture**; Music shows **ListenBrainz**. All row
  names fit and the switch/toggle geometry follows the compact profile.

Pre-existing translation gaps in the V1 asset pack (raw keys such as
`settings_section_personal…` and `musicsettings_now_playing`) are unrelated to
this port; the new tags are present in the V1 `English.ini`.

## Known gaps

The fork's `theme_set_boot_screen()` also writes a `logo:4` NAND marker that
the V1 boot script reads; that writer lives in the out-of-scope shell/theme
group. This port stores the chosen picture and boots the installed copy, but
does not write that flash marker. The fork's release-notes renderer is ported
but not wired into the existing update card. The addon list is intentionally
limited to the three modules whose gates this port enforces.

## Plugin architecture candidate

This feature group is a **plugin candidate** under the proposed extension
architecture (`docs/architecture/PLUGIN_ARCHITECTURE.md`), following the V1
fork by **@mr-f0xx** and the modular design of Compás by **@Starnished66** as
references: the modules stay self-contained, V1-gated and removable without
changing the HiBy core paths.
