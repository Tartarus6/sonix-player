# Firmware packer provenance

Audit note for the three-device packaging work. It records where each packer
came from and exactly what was changed. It is not a claim that any image is
flashable.

## HiBy packer (R1 / R3 Pro II)

* Path: `sonix-packer/sonix_firmware_packer.sh`
* Origin: the existing HiBy Sonix Player repository, already present in this
  worktree before this work.
* Changes: **none**. Its logic, `MODELS` table and output names are untouched.
  The root Makefile stages a verbatim copy into a device-scoped directory and
  runs it there, so no HiBy behaviour is changed.
* Identity mechanism: `MODELS` entries carry `HiBy R1` / `HiBy R3 Pro II`, and
  the packer verifies the `device-name` that actually landed in the rootfs.

## TempoTec V1 packer (imported)

* Destination: `sonix-packer/v1/`
* Source repository: `https://github.com/mr-f0xx/sonix-player-tempotec-v1`
* Source commit: `e96272022f53864f9ed87f615b59c922f201c060` (2026-11-22)
* Local snapshot used for the copy: `/tmp/sonix-investigation/v1-fork/sonix-packer/`
* Files imported:
  * `sonix_firmware_packer.sh` — V1-only packer; preserves the stock kernel and
    stock package files, rewrites only the rootfs. Output `v1.upt` + `v1_md5.txt`.
  * `prepare_v1_stock.sh` — joins split stock archives and installs the expected
    `v1_original.upt` name.
  * `verify_upt.sh` — structural verifier with optional `--stock` byte comparison.
  * `extract_base_build.py` — reads the stock TempoTec product version.
  * `tools/cst8xx_wake_patch.py` — from the fork's `sonix-player/tools/`; the
    V1 packer calls it to patch the stock touch module's phantom-wake handler.
  * `assets/V1/` — device overlay (136 files): `system-info.json`, boot logos,
    fonts, GUI resources, CA bundle and the V1 Bluetooth init no-op.
  * `firmware-input/README.md` — upstream description of the official 1.3 OTA.

### Changes made to the imported files

1. A `PROVENANCE:` header comment was added to each imported script and to the
   firmware-input README.
2. `sonix_firmware_packer.sh` and `verify_upt.sh`: the wake-patch tool lookup
   was made robust to the new depth. In the fork,
   `SCRIPT_DIR/../sonix-player/tools/...` pointed at the repository tree; here
   the scripts are staged beside `tools/cst8xx_wake_patch.py`, so that path is
   tried first, with the old paths kept as fallbacks. This affects only how the
   helper is located, not what the packer writes.
3. The proprietary split stock parts (`v1.zip.part1`, `v1.zip.part2`) were
   copied locally into `sonix-packer/v1/firmware-input/` for builds but are
   **ignored by git** and were not committed.

## Isolation between devices

Each root target builds in its own `build/firmware/<device>/` staging tree:
a verbatim copy of the right packer, only that device's asset tree, only that
device's verified stock input, and the single shared `sonix_player` /
`sonix_launch` built once by `make -C sonix-player target`. No image can see
another device's stock firmware or assets.

## Safety note

`tools/validate_upt.py` reports structural PASS/REJECTED only. A PASS means the
ISO, hash chains, manifest, rootfs identity and expected-device check are
consistent. It is **not** a flashability approval and says nothing about
hardware compatibility, bootloader behaviour or recovery. No image in this
repository should be flashed without device-specific testing and a retained
copy of the matching official firmware.
