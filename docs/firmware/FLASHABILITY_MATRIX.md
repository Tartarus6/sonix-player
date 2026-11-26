# Flashability matrix

Values: **PASS** (executed and observed here), **SOURCE ONLY** (supported by
project source/release metadata but not independently executed on hardware or
against an official reference), **NOT TESTED**, **NOT APPLICABLE**, **FAIL**.
An empty cell is never used. "Structural PASS" from `tools/validate_upt.py` is
not flashability approval.

|Firmware image|R1|R3 Pro II|V1|
|---|---|---|---|
|Target filename|`r1.upt`|`r3proii.upt`|`v1.upt`|
|Image built by `make <device>`|PASS (`r1.upt` 49,932,288 B)|PASS (image built, but gate REJECTED)|PASS (`v1.upt` 49,539,072 B)|
|Hardware identity verified on device|SOURCE ONLY|SOURCE ONLY|SOURCE ONLY|
|Package format verified (ISO 9660, metadata)|PASS (r1.upt parsed)|FAIL (independent review: `ota_compatibility.in` absent)|PASS (`v1.upt` parsed; ISO 9660 and required OTA metadata present)|
|Device metadata verified|PASS (`HiBy R1` in rootfs)|PASS (independent: `HiBy R3 Pro II` in generated rootfs)|PASS (`TempoTec V1` in generated rootfs)|
|Components inspected|PASS (kernel + rootfs, 6 + 89 chunks)|PASS (independent: kernel/rootfs integrity)|PASS (independent: stock kernel/rootfs integrity)|
|Checksums validated (internal consistency only)|PASS (MD5 chain, manifest, per-component MD5 declared in image)|PASS (independent)|PASS (independent, stock)|
|Compatibility metadata present|PASS (`hiby_r1_x1600_auto_rf_auto`)|FAIL (marker absent from stock and generated image)|PASS (`tempotec_v1_x1600_auto`, stock marker preserved in generated image)|
|Signature requirements understood|SOURCE ONLY (no signature scheme found; vendor requirement unknown)|SOURCE ONLY|SOURCE ONLY|
|Wrong-device rejection tested|PASS (r1.upt rejected as r3proii and as v1)|PASS (same negative control, independently repeated)|PASS (stock V1 rejected as Sonix firmware)|
|Corrupted-image rejection tested|PASS (truncated + byte-corrupt rejected)|PASS (independent: truncated and byte-flip rejected)|NOT TESTED|
|Update mechanism verified|SOURCE ONLY (OTA/Recovery code read; not executed)|SOURCE ONLY|SOURCE ONLY|
|Recovery procedure documented|SOURCE ONLY (README + bootmode script)|SOURCE ONLY|SOURCE ONLY|
|Independent safety review|NOT VERIFIED FOR FLASHING|**REJECTED** (mandatory metadata FAIL)|NOT VERIFIED FOR FLASHING|
|Physical installation tested|NOT TESTED (no device)|NOT TESTED (no device)|NOT TESTED (no device)|
|**Final flashability classification**|**NOT VERIFIED FOR FLASHING**|**REJECTED**|**NOT VERIFIED FOR FLASHING**|

## Evidence for each PASS

* **Package format / components / checksums:** `python3 tools/validate_upt.py
  sonix-packer/r1.upt --expect-device r1` → `STRUCTURAL PASS`, components
  `kernel` (2,940,992 B, 6 chunks) and `rootfs` (46,575,616 B, 89 chunks);
  report `docs/QA/validation/r1-generated.json`. ISO magic `CD001` at `0x8001`;
  `ota_config.in`, `ota_update.in`, `ota_compatibility.in`, `ota_v0.ok` present.
* **Device metadata:** same report, `observed_device: HiBy R1`, read from
  `squashfs-root/usr/resource/sonix/components/system-info.json` inside the
  reassembled rootfs.
* **Wrong-device rejection:** same image with `--expect-device r3proii` and
  `--expect-device v1` → `REJECTED`, check `wrong_device_rejection = FAIL`.
  Reports: `r1-vs-r3proii.json`, `r1-vs-v1.json`.
* **Corrupted-image rejection:** a 40,000,000-byte truncation → `REJECTED`
  (`iso_readable = FAIL`); a single byte zeroed at offset 3,000,000 → `REJECTED`
  (`rootfs_chain = FAIL`). Reports: `r1-truncated.json`, `r1-corrupt.json`.
* **Stock image structural check:** `r1_original.upt` passes ISO, kernel and
  rootfs integrity but is `REJECTED` for device identity because a stock image
  has no `system-info.json`. Report `r1-original.json`. This is expected
  behaviour, not a defect in the validator.

## Independent safety reviewer verdict

An independent reviewer, instructed not to rely on the build engineer's
conclusions, re-ran the validator and performed additional primary inspection:

* R1 generated `r1.upt`: STRUCTURAL PASS, `HiBy R1`, compat
  `hiby_r1_x1600_auto_rf_auto`, kernel 6 chunks / 2,940,992 B md5 `b48580b0…`,
  rootfs 89 chunks / 46,575,616 B md5 `5cc32df7…`. Classification:
  **NOT VERIFIED FOR FLASHING**.
* R3 Pro II: stock and generated images both **FAIL** the mandatory
  `required_metadata` / `compatibility_present` checks because
  `ota_compatibility.in` is absent; the generated image also differs from the
  official v1.3.1 digest. Classification: **REJECTED**. A failed mandatory check
  was not downgraded.
* V1 (at the time of the independent review): no Sonix `v1.upt` existed yet.
  Stock 1.3 split parts were joined and their digests matched the fork README,
  but the validator rejects the stock image for missing generated-identity
  metadata. Classification: **NOT VERIFIED FOR FLASHING**.
* The images inspected by that reviewer were pre-implementation local rebuilds:
  `r1.upt` (49,936,384 B, `ada3415b…`) and a generated R3 Pro II image
  (48,025,600 B, `3b4d2242…`). Neither matched an official v1.3.1 asset
  (`b78cb5a5…`, `afee07d4…`). They are not the images produced by `make` in this
  branch.
* Images produced after the implementation: `r1.upt` 49,932,288 B
  (`7ee51491…`) and `v1.upt` 49,539,072 B (`fb142c3d…`), both STRUCTURAL PASS
  with correct device identity; the R3 Pro II image is built but REJECTED by the
  same mandatory metadata check. These are new local rebuilds, not release
  artefacts, and remain NOT VERIFIED / REJECTED for flashing.
* The validator checks only each component's MD5 declared *inside the image*;
  it does not check kernel architecture/model, rootfs-to-partition fit,
  compatibility value, signatures, or vendor authenticity. Device identity is
  added by the Sonix overlay, so stock images have none.

The Principal Architect accepts these verdicts. They agree with the local
findings except that the independent review detected the R3 Pro II mandatory
FAIL and therefore raised its classification from NOT VERIFIED to REJECTED.

## Why no device is VERIFIED FOR FLASHING

The static and structural checks are a necessary subset, not the whole safety
case. For every device the following mandatory items remain unknown:

* no published vendor checksums or official flashing documentation were located
  for the stock reference images;
* no cryptographic signature scheme or keys exist in the project, and it is
  unknown whether the device requires one;
* source indicates the recovery kernel performs no model or signature check, so
  a mis-targeted image could be accepted by the device;
* no physical device, boot test, or USB recovery test was available;
* the official R1/R3 Pro II release assets were not downloaded or inspected;
* a V1 Sonix image was produced and passed structural validation, but it was
  not installed or inspected on a V1 device.

Do not flash any of these images. Explicit "do not flash" cases: the generated
R3 Pro II image; a local `r1.upt` presented as v1.3.1; any image presented under
a device name other than its observed identity; and all stock images presented
as Sonix firmware.
