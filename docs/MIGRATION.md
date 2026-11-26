# OTA compatibility and migration risks

## How OTA selects a firmware today

`src/system/device/ota.c` queries the GitHub Releases API for the repository
named in `system-info.json`'s `ota-repo` field, falling back to a compiled
`DEFAULT_REPO`. It scans the newest 20 releases, skips drafts and prereleases,
requires a parseable version tag, and looks for an asset with the **exact** name
`<stem>.upt` where `stem` comes from `firmware.name` or the device profile
(`r1`, `r3proii`, `v1`). It checks the asset's declared `size`; it verifies the
`sha256:` digest **only when the release publishes one** (`ota.c:479`; absent
digests are logged as "not published" at `ota.c:295-296`). `src/system/device/firmware.c` refuses to
arm an update for an unknown device because there is no stem.

Consequences of the current distribution layout:

* the repository owner/name, asset filename, release tag format and the
  "published (not draft/prerelease)" state are all part of the update contract;
* moving firmware to a new canonical repository without updating every
  `system-info.json` would silently disable OTA on existing installations;
* changing the `device-name` or the JSON layout can break model detection,
  leaving the device with no stem and no update path;
* only the 20 newest releases are visible, so an older pinned release would not
  be found;
* the local `system-info.json` currently points at `Jepl4r/sonix-player`, which
  returns HTTP 404. That is a real, existing migration defect for local builds.

## OTA wrong-device risk is reduced, not eliminated, by release hygiene

Selection is keyed by the stem derived from the device profile, and the packer
independently refuses to build an image whose overlay `device-name` differs
from the expected model. This work added a third, independent check in
`tools/validate_upt.py`: the `device-name` inside the rootfs is compared with the
requested device and the image is rejected on mismatch.

These checks **reduce risk but do not guarantee safety**. All three are
software-side and run before installation: the first two at release/build time,
the third only when someone runs the validator. None is part of the on-device
OTA client or the recovery kernel, which source indicates does not verify device
identity or signatures. If a wrongly named `<stem>.upt` were published in a
release, the client would download it. Correct release hygiene and an
independent pre-publication check are therefore mandatory.

## Migration requirements for this integration

1. If the canonical repository changes, update `ota-repo` in every
   `assets/<model>/usr/resource/sonix/components/system-info.json`, and ensure
   the new repository publishes assets named exactly `r1.upt`, `r3proii.upt`
   and `v1.upt` in non-draft, non-prerelease releases.
2. Keep a compatible release-tag scheme; non-numeric tags are skipped by the
   client version parser.
3. Preserve the published SHA-256 digest for each asset so the client can
   verify downloads.
4. Treat V1 separately: its fork points at `mr-f0xx/sonix-player-tempotec-v1`
   and its stock 1.3 kernel requirement must survive any migration.
5. Do not publish a firmware release as part of this task. Release
   infrastructure is a proposal only.

## Open risks

* No OTA update was executed in a live or simulated update flow.
* Whether old installations can migrate to a new repository without a manual
  configuration change is unknown; the client reads the value at runtime from
  the rootfs, so a firmware update would be required to change it.
* Recovery from a failed OTA is documented only from source; see
  `docs/firmware/RECOVERY_GUIDES.md`.
