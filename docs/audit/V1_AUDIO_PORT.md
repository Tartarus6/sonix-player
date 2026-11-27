# V1 audio feature group — port audit

Base: `pr/three-device` at `f46edd1220fca0306fa83612a2ea3efab4a2a1d0`.
Source: the TempoTec V1 fork by **@mr-f0xx**, commit `335361a`
(`.v1-source/sonix-player`). All design and measurement credit for the
tape deck, sampler, recorder, USB host work and visualizer belongs to
@mr-f0xx; this change is an adaptation to the unified three-device tree.

**No image produced by this branch is safe to flash.** All verification was
done in the host SDL simulator or by cross-compilation. No USB role forcing,
PMIC, charge or DAC hardware behaviour was exercised on real hardware.

## Imported files

`sonix-player/src/gui/tapedeck/`
`tapedeck.c`, `tapedeck.h`, `tapesampler.c`, `tapesampler.h`, `tapewidgets.h`.

`sonix-player/src/system/audio/`
`tapemix.c/.h`, `tapepads.c/.h`, `taperecorder.c/.h`, `tapestore.c/.h`,
`tapewav.c/.h`, `visualizer.c/.h`, `usbhostmode.c/.h`, `usbhostprobe.c/.h`.

No games, library, streaming, shell or settings groups were imported.

## Model gating (hardware safety)

`usbhost_is_v1()` in `usbhostprobe.c` reads `sysinfo_model()` and accepts only
`update_stem == "v1"` or `compact_panel`. Every path that can force the
Type-C controller to Source, write the x1600 PHY's `sw_switch_hsotg`, supply
VBUS, or write a recovery journal is behind it: `usbhostprobe_begin()`,
`usbhostprobe_begin_held()`, `usbhostprobe_recover()`, `request_host_locked()`,
`restore_locked()` and `keep_role_locked()`. `usbhostmode_tick()`,
`usbhostmode_set_wanted()` and `usbhostmode_release()` also return on non-V1.
The local `usb_external_host_*()` coordination shims are no-ops off the V1.
R1 and R3 Pro II therefore perform no new hardware writes: the new code paths
return `USBHOST_PROBE_UNSUPPORTED` or do nothing before any sysfs access.

## API changes / backward-compatible adaptations

* Theme: the unified `theme.h` has no semantic roles. `tapewidgets.h` adds
  `tape_tone_color()` / `tape_tone_t` for the meter's amber/red accents.
* Geometry: `settingsrow_header_text_left()` and
  `settingsrow_corner_button_size()` do not exist in the unified shell;
  `tape_header_left()` / `tape_corner_size()` use `bp_pick()` and `cfg`
  dimensions instead. The compact page body is raised by `bp_pick(20, 0)`
  and compact pad rows use `bp_pick(36, 60)` so all four rows fit 240x320.
* PCM: the fork's owner-token API and `audio_stop_and_wait()` are absent.
  `tapepads.c` uses `audio_external_begin_yielding()`,
  `audio_external_wanted()`, `audio_external_write()`, `audio_external_end()`
  and a local stop-and-wait helper.
* Storage: the fork's `storage_card_file_job_begin/end()` are absent;
  `tapewav.c` supplies `tape_card_job_begin/end()` shims using
  `storage_sd_root()`.
* USB coordination: the fork's `usb_external_host_*()` live in its `usb.c`,
  outside this scope; V1-only local shims replace them without touching
  shared device code.
* A host-only QA hook in `tapedeck.c` opens the page when
  `SONIX_QA_TAPEDECK=1` is set (`=pads` selects the pad grid). It is compiled
  out of target firmware and is not a shell menu integration.

## Build and visual verification

`make -j4 target` in the copied toolchain/build environment: **exit 0**,
`sonix_player` linked with all ten new translation units. Host build with the
provided `PKG_CONFIG_PATH`/`LDFLAGS`: **exit 0**.

Host SDL simulator captures under bubblewrap at all three panel sizes were
opened and inspected. Exact dimensions were checked with PIL:

| Device | Panel | Evidence |
| --- | --- | --- |
| HiBy R1 | 480x800 | `docs/QA/screenshots/r1/audio.png` |
| HiBy R3 Pro II | 480x720 | `docs/QA/screenshots/r3proii/audio.png` |
| TempoTec V1 | 240x320 | `docs/QA/screenshots/v1/audio.png` |

Each shows the deck's Memos view with functioning tabs, meter, transport and
empty memo state. The Pads view was also inspected on all three panels; a
compact-only body offset and 36 px pad rows were added after the first V1
capture showed the fourth row clipped. Simulator evidence only.

## Plugin direction

This group is a plugin candidate under the proposed extension architecture
described in `docs/architecture/PLUGIN_ARCHITECTURE.md`: it stays self-contained
behind the existing audio/UI interfaces, with its V1-only hardware behaviour
gated at runtime, so it can eventually be loaded or omitted without changing
core behaviour. Reference points are @mr-f0xx's TempoTec V1 fork, from which
the code was ported, and @Starnished66's Compás modular design, as the
architectural precedent for optional feature groups.
