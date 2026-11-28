# Firmware build outputs (local artefacts)

These images were produced by `make r1`, `make r3proii` and `make v1` in this
worktree. They are **git-ignored build artefacts**, not committed source, and
are not release assets. A structural PASS is **not** flashability approval.

| Device | File (local) | Bytes | SHA-256 | Structural validation | Flashability |
|---|---|---|---|---|---|
| HiBy R1 | `build/firmware/r1/r1.upt` | 49,932,288 | `7ee51491dfc8edc0f532b859fd6395c3775bdd55947f177cbd2f3d1eff6af4fe` | STRUCTURAL PASS, device `HiBy R1` | NOT VERIFIED FOR FLASHING |
| HiBy R3 Pro II | `build/firmware/r3proii/r3proii.upt` | 47,640,576 | `b0e7bca2b475a6a0a4e2d8edb2c481b734ed319b45ba1fde42cf86929f65080b` | **REJECTED** — `required_metadata` and `compatibility_present` FAIL | **REJECTED — do not flash** |
| TempoTec V1 | `build/firmware/v1/v1.upt` | 49,539,072 | `fb142c3dcdbe02fe65a7763c39152dae465a976ecb9d13e889e21f0cc6ec7ad5` | STRUCTURAL PASS, device `TempoTec V1` | NOT VERIFIED FOR FLASHING |

## Why R3 Pro II is rejected

The only official stock image available (downloaded from
`oss.hiby.com/HiBy R3PROII/en-us/1.4/…` and confirmed byte-identical to the
local `r3proii_original.upt`, SHA-256 `6ef350aa4a513201b8376768ed8c7593cf16ae84ae3f458c8a5338adf0420dbf`)
contains no `ota_v0/ota_compatibility.in`. The build gate refuses to relax that
check: inventing a compatibility marker would be fabricating firmware metadata.
A newer official stock image carrying the marker is required before this image
can even reach STRUCTURAL PASS.

## Reproduction

```
make r1       # exit 0
make v1       # exit 0
make r3proii  # exit 2 — mandatory gate, as expected
python3 tools/validate_upt.py build/firmware/r1/r1.upt --expect-device r1
python3 tools/validate_upt.py build/firmware/v1/v1.upt --expect-device v1
```

No image may be flashed without a physical-device test and a retained copy of
the matching official firmware. See `FLASHABILITY_MATRIX.md` and
`RECOVERY_GUIDES.md`.
