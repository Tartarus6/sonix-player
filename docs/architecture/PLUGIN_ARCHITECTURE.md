# Proposal: an extension architecture for Sonix Player

**Status:** proposal for maintainer review. No runtime is implemented by this
document. It responds to the maintainer's comment on PR #28 that a plugin
system would be preferable to adding bespoke features to the core.

## Why

The TempoTec V1 fork by **@mr-f0xx** (`mr-f0xx/sonix-player-tempotec-v1`) carries
a large set of optional features — games, calculator, notes, pet, tape deck,
Jellyfin/SoundCloud/Subsonic clients, ListenBrainz, plugins/settings pages,
gestures and volume readout. Porting all of them into the core would balloon
the binary, the image size and the maintenance burden on the X1600's limited
resources. The Compás player by **@Starnished66**
(`Starnished66/compas-player`) solves this with a modular architecture: plugins,
themes and Now Playing layouts are independent extension points.

## Reference model (Compás, credited)

Compás implements:

* a Lua runtime: `src/plugins/lua/plugin_lua_runtime.c/.h`;
* bindings for audio, IO/filesystem, library and UI:
  `src/plugins/bindings/{audio,io,library,ui}/`;
* manager and API versioning: `src/plugins/manager/plugin_api_version.h`,
  `plugin_call.c`, `plugin_events.c`, disabled-plugin list;
* an on-device Plugin Store: `src/network/store/plugins/`;
* separate extension points for themes and LVGL XML Now Playing layouts;
* native external apps under `SD/.apps/`.

A plugin is a single Lua file in `SD/.plugins/`, registered with
`plugin.define({ id, name, version, api_min })` and
`plugin.register_list_item("settings", ...)`. The user enables/disables and
updates plugins from `Settings → System → Plugin Manager`. The runtime contains
a reserved-path check (`plugin_lua_check_external_path`) so a plugin cannot
read arbitrary firmware paths.

This design is the reference, not something to copy blindly: Sonix's build is
C/LVGL with a vendored toolchain and no Lua today.

## Proposed staging for Sonix

### Stage 1 — extension points in the core (small, reviewable; compiled-in only)

1. A manifest format for optional features:
   `SD/.sonix/extensions/<id>/manifest.ini` with `id`, `name`, `version`,
   `api_min`, `kind=ui|service|native`, `entry`. In Stage 1 `kind` only selects
   which built-in hook is used; it does not authorise loading external code.
2. Registration hooks at stable places that already exist: Settings rows
   (`sonix-player/src/gui/settings/`), More-page tiles
   (`sonix-player/src/gui/shell/morepage.h`) and Now Playing menu entries
   (`sonix-player/src/gui/nowplaying/`). The service tick is different: it is a
   new hook Stage 1 must introduce, not an existing integration point. It runs
   on a bounded worker thread and hands results to the GUI through the existing
   queued `gui_post` path; blocking HTTP or file work must not run on the
   `lv_timer_handler()` loop in `sonix-player/src/main.c` (PR #95 needs this
   because `http_get()` blocks). No hook is invoked unless a manifest is
   present and enabled.
3. An enable/disable list persisted in the existing config store, with a
   System page listing discovered extensions and their API versions.
4. Capability gating: an extension declares required device capabilities
   (`wifi`, `touch`, `dac`, plus `compact_panel` only where a compact/V1 model
   exists) and is hidden on devices that do not provide them. Capability
   discovery is Stage 1 work: this base's device table
   (`sonix-player/src/system/device/sysinfo.c`) knows only R1 and R3 Pro II,
   so V1/compact-panel gating depends on PR #24's three-device model landing
   first. With that dependency met, each device gets appropriate layouts.
5. Failure containment: an extension whose manifest fails validation is
   reported and disabled; a service that blocks or overruns its budget cannot
   block playback or OTA.

Stage 1 covers compiled-in extensions gated by manifests: no external Lua or
native `.so` code is loaded or executed until Stage 2 defines the runtime and
ABI. PR #95's Phase 2 sync client is therefore a compiled-in service using
these hooks, not a dynamically loaded plugin. Because it is part of the
firmware image, changing it requires a firmware rebuild and reflash; manifests
only gate and configure it, they do not download or load new code. For the same
reason PR #95 §5.2's "load failure disables only this extension" describes
Stage 1 manifest/config validation and failure containment, not a code loader:
a real load step arrives in Stage 2.

### Stage 2 — safe runtime

Evaluate Lua (as Compás does) versus a restricted native `.so`/fixed-ABI
plugin. Requirements: bounded memory and CPU on the X1600, sandboxed file and
network access, no direct framebuffer or MTD access, and a versioned API that
rejects incompatible plugins instead of crashing. The Stage 2 ABI decision is
also what would turn a `kind=native` manifest into a loadable extension.

### Stage 3 — distribution

A catalog and on-device store, signature/checksum verification, and separate
updates without a firmware reflash — for **Stage 2 loadable extensions only**.
Stage 1 extensions are compiled into the firmware image and cannot be updated
from a catalog: changing one requires a firmware rebuild and reflash. PR #95's
Phase 2 sync client is such a compiled-in Stage 1 service, so its Phase 3 plan
to "update the player's sync client without a reflash" is not achievable as
written: that phase must either move the client to a Stage 2 runtime extension
or drop the no-reflash promise.

The catalog reuses the in-tree SHA-256 OTA verification in
`sonix-player/src/system/device/ota.c` and
`sonix-player/src/system/core/sha256.c` instead of inventing a second path.
`tools/validate_upt.py` is not in this tree; on the unmerged V1/integration
branches it verifies an MD5 chunk chain and is not a security guarantee, so it
must not be reused as extension integrity (aligned with PR #95 Phase 3).

## Relationship to the V1-fork features

The six feature groups currently being evaluated from @mr-f0xx's fork map to
extension candidates:

| Group | Suggested kind | Why not core |
|---|---|---|
| games, calculator, notes, pet | UI extension | Optional, large asset sets |
| tape deck, visualizer, USB host audio | service + UI | Hardware-specific, power risk |
| Jellyfin, SoundCloud, Subsonic, Qobuz genres | service | Network maintenance burden |
| album grid, listening stats, genius | UI extension | Alternative browsing modes |
| plugin manager, ListenBrainz, battery profile pages | settings extension | Niche |
| gestures, volume readout, shell icons | UI extension | Device-specific input |

Porting them into the core is deferred until Stage 1 exists; a small number
that are universally useful can still be proposed as core PRs individually.

## Safety and licence notes

* No firmware image is safe to flash on the basis of this document.
* Compás is GPL-3.0; Lua and any runtime dependency must be licence-reviewed
  before integration. Attribution: @Starnished66 for the Compás design and
  @mr-f0xx for the V1 fork's feature set.

## Decision requested

Does the maintainer want (a) Stage 1 implemented as a first PR, with a chosen
runtime in Stage 2, or (b) only the extension-point hooks now and the runtime
discussed separately? No code will be added to the core before that answer.
