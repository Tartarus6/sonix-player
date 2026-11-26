# Recovery guides (source-derived, not hardware-validated)

These procedures are taken from the project README, in-app code and V1 fork
documentation. They have **not** been executed on hardware. They are recorded so
that a future physical test has a documented rollback route, not as a guarantee
that recovery is possible.

## HiBy R1 / R3 Pro II

Source evidence:

* README describes a failsafe combination: hold **Volume Up + Power** until the
  HIBY logo appears. The exact timing and whether it works on both units is not
  independently verified.
* In-app update (`src/system/device/firmware.c`) arms recovery by invoking
  `bootmode.sh Recovery`, which writes `ota:kernel2` into the first block of
  `/dev/mtd5`, then reboots.
* `ota.c` downloads the selected `<stem>.upt` to `<stem>.upt.part`, checks size
  size and verifies the SHA-256 digest only when the release publishes one
  (`ota.c:479`); it removes old update files and renames before rebooting.
* README states recovery is to reflash the vendor stock `.upt` image. No vendor
  download URL or published checksum was located for the stock R1/R3 images in
  this work.

Preconditions that must be established before any physical test:

* exact device model and current firmware version;
* a verified vendor stock image and its checksum, stored locally;
* battery/power conditions per vendor guidance (not established here);
* a written record of what the failsafe combination does on that unit.

Do not experiment with the bootloader, `/dev/mtd5`, partition tables or
recovery partitions on a physical device as part of an automated test.

## TempoTec Variations V1

Source evidence:

* The V1 fork's README warns to retain the stock firmware and its checksum.
* Its packer preserves the stock package file-by-file (`verify_upt.sh --stock`)
  and does **not** replace the kernel; the stock 1.3 kernel is required because
  it adds SPI NAND support for the `ZB35Q01CYIG` chip.
* The fork bundles the official V1 1.3 OTA split parts and records their SHA-256
  values in `firmware-input/README.md`.
* Its CI explicitly states it does not install or re-test the generated image on
  physical hardware.

No V1 device, installation method or recovery procedure was validated here.
The stock V1 1.3 package must be available and verified before any test.

## Recovery limitations

* Whether the failsafe combination works after a failed kernel/rootfs update is
  unknown.
* Whether a USB recovery path exists on any of the three devices is unknown.
* Whether the bootloader or recovery checks device identity or signatures is
  unknown; source indicates it does not.
* A failed update could leave a device unbootable without a vendor tool. Do not
  assume recovery is guaranteed.

## Before any physical flashing (not authorised here)

Provide, and have independently checked: exact device model, exact filename,
SHA-256 checksum, current firmware version, installation method, power
requirements, storage preparation, backup, recovery procedure, known risks and
post-install verification steps. Physical flashing must never be automated and
requires separate explicit user authorisation.
