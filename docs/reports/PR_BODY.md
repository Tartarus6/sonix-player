# Proposed PR body (draft — not published)

**Title:** `Unified three-device Sonix Player: V1 support, per-device builds and firmware safety gate`

**Target:** `Tartarus6/sonix-player` — branch `main`
**Source:** `CookieCaptainD/sonix-player` — branch `integration/three-device`

> This contribution implements V1 device support in the shared codebase, plus
> per-device build targets, CI and a mandatory firmware validation gate. It does
> **not** claim physical-device validation or flashability; the R3 Pro II target
> is blocked by a missing compatibility marker in the available stock image.

## Summary

This branch preserves the existing working tree and adds:

* a full inventory of the local repository, upstream and the TempoTec V1 fork,
  with commit SHAs and feature provenance;
* a neutral comparison of competing implementations and an item-by-item decision
  for every local improvement absent from the V1 fork;
* an independent, read-only `.upt` validator that checks ISO structure, OTA
  metadata, MD5 chains, manifests, component MD5s and rootfs device identity;
  wrong-device, truncated and corrupted inputs are rejected in executed tests;
* per-device flashability documentation, including an independent safety review;
* genuine host-simulator screenshots for all three panels (480×800, 480×720,
  240×320) and a complete test report.

## Device status

| Device | Build in this branch | Flashability |
|---|---|---|
| HiBy R1 | `make r1` PASS | NOT VERIFIED FOR FLASHING |
| HiBy R3 Pro II | build blocked by gate | REJECTED (mandatory compatibility metadata absent) |
| TempoTec V1 | `make v1` PASS | NOT VERIFIED FOR FLASHING |

No image in this contribution is safe to flash. No signature scheme was found;
recovery procedures are documented from source only and were not tested.

## Deliberately out of scope here

V1-fork-only features beyond device support (themes, DLNA, lyrics overlay,
listening statistics, tape deck, extra games) are **not ported**, and neither is
physical-device validation or an OTA repository fix. The R3 Pro II build needs a
stock reference carrying `ota_compatibility.in`. See the report for details.

## Documentation

Complete engineering report (Markdown):
[docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.md](docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.md)

Complete engineering report (PDF, all evidence):
[docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.pdf](docs/reports/SONIX_PLAYER_INTEGRATION_REPORT.pdf)

Key documents:
[flashability matrix](docs/firmware/FLASHABILITY_MATRIX.md) ·
[flashing safety](docs/firmware/FLASHING_SAFETY.md) ·
[recovery guides](docs/firmware/RECOVERY_GUIDES.md) ·
[test report](docs/QA/TEST_REPORT.md) ·
[UI review](docs/QA/UI_REVIEW.md) ·
[implementation comparison](docs/audit/IMPLEMENTATION_COMPARISON.md)

## Validation

`python3 tools/validate_upt.py <image>.upt --expect-device r1|r3proii|v1`
Exit 0 means structural checks passed — explicitly **not** flashability approval.
Reports: `docs/QA/validation/`.

## Publication and flashing

Nothing has been pushed or released. Opening this PR and any physical flashing
require separate explicit authorisation from the repository owner.
