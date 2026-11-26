# Building

This documents the build environment **as observed**, including failures. No
firmware image was built by this integration work; the validation described in
`docs/QA/TEST_REPORT.md` was run against images that already existed in the
local tree.

## Layout

* Repository root: this worktree (the directory holding `Makefile`).
* Application sources and Makefile: `sonix-player/`.
* HiBy packer: `sonix-packer/sonix_firmware_packer.sh` (unchanged).
* Device assets: `sonix-packer/assets/R1/`, `sonix-packer/assets/R3PII/`.
* Imported V1 packer, helpers, assets and input notes: `sonix-packer/v1/`
  (see `docs/audit/PACKER_PROVENANCE.md`).
* Per-device staging: `build/firmware/<device>/`, created by the root Makefile.
* Validator: `tools/validate_upt.py`; stock-input gate:
  `tools/prepare_stock_inputs.sh`.

## Application build

`sonix-player/sonix-player/Makefile` targets:

| Target | Output | Notes |
|---|---|---|
| `make all` | host binary + compile commands | default |
| `make host` | `sonix_player_host` | SDL2 simulator; needs system `opusfile` and `wavpack` dev packages |
| `make target` | `sonix_player`, `sonix_launch` | cross-compiles with `rockbox-toolchain`, then `check-abi` and `strip-target` |
| `make check-abi` | — | refuses symbols newer than device glibc 2.22 |
| `make test-axpcharge` | test binary | charger state-machine test |
| `make libs-clean` | — | removes downloaded/built libraries |

There are **no** `r1`, `r3proii` or `v1` make targets. The same binary is built
once and the device is selected at runtime by `system-info.json` (see
`DEVICE_COMPATIBILITY.md`). The V1 fork adds `web-emu`, `run-web-emu`, `web-wasm`
and `web-wasm-bundle` targets.

Observed build attempt in the integration worktree (2026-11-22):

```
cp <original-tree>/*.tar.* sonix-player/
make host
```

Result: **FAILED**, exit 2. `pkg-config` could not find `opusfile` or `wavpack`
and `src/system/decode/opusdec.c:6` failed with `opusfile.h: Ficheiro ou pasta
inexistente`. The host build expects those libraries to be installed system-wide
(`libopusfile-dev`, `libwavpack-dev`); the vendored tarballs in the repository
are used for the **target** build only. A prebuilt `sonix_player_host` binary was
present in the original tree and was used for screenshot capture instead.

## Firmware packaging (existing process)

```
cd sonix-packer
# requires r1_original.upt and/or r3proii_original.upt next to the script
./sonix_firmware_packer.sh
```

The packer reads the `MODELS` table, skips models whose stock image is missing,
unpacks the ISO, swaps the player/launcher, overlays per-model assets, rewrites
the squashfs, rebuilds the OTA chunks and writes `r1.upt` / `r3proii.upt`. The
V1 uses the imported `sonix-packer/v1/sonix_firmware_packer.sh`, which preserves
the stock kernel and stock package files, and emits `v1.upt` + `v1_md5.txt`.

Required tools observed: `7z`, `unsquashfs`, `mksquashfs`, `mkisofs`/`genisoimage`/
`xorrisofs`, `md5sum`, `split`, `cat`. The validator additionally uses
`python3`, `file`, `xxd` and `sha256sum`.

## Required stock inputs (proprietary, never committed)

| Device | Input | Where to put it locally |
|---|---|---|
| R1 | `r1_original.upt` | `sonix-packer/` (or `$SONIX_STOCK_DIR`) |
| R3 Pro II | `r3proii_original.upt` | `sonix-packer/` (or `$SONIX_STOCK_DIR`) |
| V1 | `v1.zip.part1` + `v1.zip.part2` (official TempoTec V1 1.3 OTA) | `sonix-packer/v1/firmware-input/` (or `$SONIX_V1_INPUT`) |

Expected SHA-256 values are recorded in
`docs/firmware/STOCK_SHA256.sha256`. `tools/prepare_stock_inputs.sh` checks every
input against that file before packing, joins the V1 parts, extracts the stock
`v1_original.upt`, and refuses to continue if anything is missing or does not
match. Copy the files in locally from your own legally obtained firmware; never
commit them (`.gitignore` ignores `*.upt` and the V1 split parts).

The V1 parts can also be supplied as a raw `v1.upt`, an archive, or a prepared
`v1_original.upt`; see `sonix-packer/v1/firmware-input/README.md` and
`prepare_v1_stock.sh`.

## Per-device targets (implemented)

```
make r1        # -> build/firmware/r1/r1.upt
make r3proii   # -> build/firmware/r3proii/r3proii.upt
make v1        # -> build/firmware/v1/v1.upt
make all       # all three; the application is built once by make -C sonix-player target
make validate  # re-run the mandatory validator on built images
```

Each target:

1. delegates the application build to `make -C sonix-player target`;
2. creates a fresh device-only staging tree under `build/firmware/<device>/`
   with a verbatim copy of the right packer, only that device's assets and
   kernel (where used), and the `sonix_player` / `sonix_launch` binaries;
3. stages the stock input only after `tools/prepare_stock_inputs.sh` verifies
   its SHA-256;
4. runs the packer from the staging tree;
5. runs `tools/validate_upt.py <image> --expect-device <device>` as a mandatory
   gate — a structural FAIL exits non-zero and rejects the image;
6. prints the output path, byte count, SHA-256 and validation result.

`make clean` removes only the staging tree. Stock inputs are never deleted or
committed.

## Known blocker: R3 Pro II stock metadata

With the only available stock input (`r3proii_original.upt`, SHA-256
`6ef350aa…`, 37,996,544 bytes), `make r3proii` packs an image with the correct
`HiBy R3 Pro II` identity and valid hash chains, but the mandatory validator
**REJECTS** it:

```
required_metadata: FAIL  missing ota_v0/ota_compatibility.in
compatibility_present: FAIL  None
```

That file is absent from the stock R3 Pro II update itself. The gate is left
strict on purpose — a structural FAIL must fail the target — so `make r3proii`
and `make all` exit non-zero until a stock R3 Pro II image that carries the
expected compatibility metadata is supplied (and its digest recorded in
`docs/firmware/STOCK_SHA256.sha256`). Do not fabricate the marker or bypass the
validator.

## Safety

The validator's PASS is explicitly `STRUCTURAL PASS (flashability NOT
established)`. It checks the ISO, OTA metadata, chunk/MD5 chains, manifest,
reassembled component hashes and the `device-name` inside the rootfs, including
rejection of the wrong device. It does **not** test hardware compatibility,
bootloader behaviour or recovery, and no image here is approved for flashing.

## Validation (implemented and run)

```
python3 tools/validate_upt.py <image>.upt --expect-device r1|r3proii|v1
```

The tool independently opens the ISO, verifies metadata, the per-chunk MD5
chain, the manifest and the reassembled component MD5s, then reads
`device-name` from the rootfs. Exit 0 means structural checks passed — **not**
that the image is safe to flash. See `docs/firmware/FLASHABILITY_MATRIX.md`.
