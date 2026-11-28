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

### Stage 1 — extension points in the core (small, reviewable)

1. A manifest format for optional features:
   `SD/.sonix/extensions/<id>/manifest.ini` with `id`, `name`, `version`,
   `api_min`, `kind=ui|service|native`, `entry`.
2. Registration hooks at stable places that already exist: Settings rows,
   More-page tiles, Now Playing menu entries and a service tick callback.
   No plugin code runs unless a manifest is present and enabled.
3. An enable/disable list persisted in the existing config store, with a
   System page listing discovered extensions and their API versions.
4. Capability gating: an extension declares required device capabilities
   (`wifi`, `touch`, `dac`, `compact_panel`) and is hidden on devices that do
   not provide them. This keeps R1/R3 Pro II/V1 layouts appropriate.
5. Failure containment: a plugin that fails to load is reported and disabled;
   it cannot block playback or OTA.

### Stage 2 — safe runtime

Evaluate Lua (as Compás does) versus a restricted native `.so`/fixed-ABI
plugin. Requirements: bounded memory and CPU on the X1600, sandboxed file and
network access, no direct framebuffer or MTD access, and a versioned API that
rejects incompatible plugins instead of crashing.

### Stage 3 — distribution

A catalog and on-device store, signature/checksum verification, and separate
updates so optional features do not require a firmware reflash. This is where
the existing OTA integrity work (`tools/validate_upt.py`, SHA-256 asset digests)
should be reused.

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
