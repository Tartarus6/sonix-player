# Architecture and engineering decisions

This log records decisions made during investigation and the rationale. A
decision being recorded does not mean it has been implemented; implementation
status is stated per item. No decision here authorises publication or flashing.

## D1 — Preserve the original working tree; integrate in an isolated worktree

The primary repository had 11 unstaged modified files and 18 untracked entries.
No destructive Git operation was used. A new branch `integration/three-device`
and worktree were created from the unmodified HEAD `7aa3637f7`. **Status:
implemented.** Rationale: the user's work must remain recoverable.

## D2 — Treat source claims as evidence, not hardware fact

Device specifications in the README and code are labelled "source-reported".
No hardware specification is promoted to a verified fact without a device test.
**Status: applied throughout the documentation.**

## D3 — Do not force one packer or one device profile

The HiBy devices use a runtime `MODELS[]` table and a packer with a two-model
`MODELS` table; the V1 fork uses a compile-time `board_profile` and a single-model
packer with stricter stock preservation and a mandatory stock kernel. Combining
them would require reconciling incompatible kernel policies and fixed UI
constants. **Decision: preserve both patterns. Status: proposal, not implemented.**

## D4 — Broadcast the strongest available browser-state implementation

Three copies exist; the V1 fork's is tracked and tested, the local uncommitted
copy is not. Decision: prefer the V1 design and replace the untracked local copy,
after comparing the committed `SNXBRW2` branch. **Status: proposal.**

## D5 — Preserve AXP charging and car-mode code per device

Both areas have independent implementations with unresolved provenance and high
regression risk. Replacing working power/Bluetooth code to achieve uniformity is
not justified. **Status: proposal; no code changed.**

## D6 — Add an independent structural validator

`tools/validate_upt.py` was written to inspect images without trusting the build
script. It verifies ISO structure, metadata, MD5 chains, manifests, component
MD5s and rootfs device identity, and performs wrong-device and corruption
rejection. **Status: implemented and executed.** Its PASS result is explicitly
labelled structural, not flashability approval.

## D7 — No image may be classified VERIFIED FOR FLASHING

No signature scheme, official flashing documentation, published vendor
checksums or physical device were available. All three devices are classified
NOT VERIFIED FOR FLASHING. **Status: applied in `FLASHABILITY_MATRIX.md`.**

## D8 — Screenshots only where genuine evidence exists

An initial host-simulator screenshot of the R1 main screen was captured from a
prebuilt binary whose exact commit is unknown and is reported as such. After the
V1 port the host binary was rebuilt locally, and genuine per-device frames were
captured for R1 (480×800), R3 Pro II (480×720) and V1 (240×320). All are
simulator renders, not physical-device evidence.
**Status: applied in `docs/QA/UI_REVIEW.md`.**

## D10 — Port V1 as a runtime profile in the shared binary

The V1 fork's `board_profile` was imported and generalised: compact geometry is
selected by the `compact_panel` flag in the model table, so one binary serves
all three devices. The fork's own history indicates its profile was originally
parameterised for three boards. HiBy constants stay unchanged: the profile only
supplies padding, status-bar height and screen size, which `main.c` and
`fonts.c` actually read. **Status: implemented** (`board_profile.c/h`, `sysinfo.c`,
`main.c`, `fonts.c`, `alsa-controls.c`). The V1 stock packer and assets were
imported under `sonix-packer/v1/` with provenance documented. V1-fork-only
features were deliberately not ported in this change.

## D9 — Do not publish

No push, tag, release, PR or firmware distribution was performed. The final
authorisation question is reserved for the end of the review package.
**Status: upheld.**
