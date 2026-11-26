# Device compatibility

All hardware facts below are **source-reported**: they come from the project's
README, `system-info.json`, driver filenames and code comments. No physical
device was available, so none is a measured hardware fact. "Verified" here means
verified in source, not verified on hardware.

| Item | HiBy R1 | HiBy R3 Pro II | TempoTec Variations V1 |
|---|---|---|---|
| Target image | `r1.upt` | `r3proii.upt` | `v1.upt` |
| Local support | V (`assets/R1`) | V (`assets/R3PII`) | **implemented** (model row + `sonix-packer/v1/assets/V1`) |
| V1 fork support | absent | absent | V (`assets/V1`), only model; source of the port |
| Panel (source) | 480×800 | 480×720 | 240×320 |
| SoC family (source comments) | Ingenic X1600E | Ingenic X1600E | Ingenic X1600 |
| Kernel (source) | custom 4.4.94, packaged `xImage-R1` 2,940,992 B | custom 4.4.94, `xImage-R3PII` 2,973,760 B | stock 1.3 kernel retained, never replaced |
| DAC (source) | single CS43131 | dual CS43198 | dual CS43131 (deliberate routing flag) |
| Touch (source driver) | Hynitron CST8xx | Goodix GT9xx (up to 5 fingers) | CST8xx (patched) |
| PMIC (source) | AXP2101 | MP2731 | AXP2101 (V1 fork charge work) |
| Buttons (source) | right flank only | both flanks, media keys swapped in kernel | swapped media keys, headset switch idle=1 |
| Audio module | `x1600_hiby_r1_sound_card.ko` + `codec_cs43131.ko` | `x1600_hiby_r3proii_sound_card.ko` + `codec_cs43198_dual.ko` | stock modules preserved |
| Display module | `lcd_lg35583.ko` | `lcd_st7701_sbtc033001.ko` | stock modules preserved |
| Stock reference image present locally | V (`r1_original.upt`, 44,255,232 B) | copies in other worktrees (e.g. `sonix-build/sonix/…`, 37,996,544 B); **not in primary tree** | V1 1.3 OTA split parts in the V1 fork |
| Official built image accessible | GitHub release v1.3.1 `r1.upt` 47,736,832 B | GitHub release v1.3.1 `r3proii.upt` 48,046,080 B | V1 fork release 1.7.3 `v1.upt` 51,300,352 B |
| Structural validator run on a generated image | PASS (`r1.upt`, `HiBy R1`) | NOT TESTED (no image in primary tree) | NOT TESTED (no local image) |
| Flashability classification | NOT VERIFIED | NOT VERIFIED | NOT VERIFIED |

## Shared architecture

The local/upstream codebase ships **one binary**. Device selection is runtime:
`device-name` in `usr/resource/sonix/components/system-info.json` is looked up in
`sysinfo.c`'s `MODELS[]`; a framebuffer-size match is a fallback. This already
avoids duplicating the application per device. Hardware differences are encoded
as model flags (`cs43131`, `pmic_charger`, `tap_wake`, `one_flank`,
`media_keys_swapped`) and per-model asset trees with model-specific `.ko` files.
The V1 fork takes the opposite approach: a compile-time `board_profile` and a
single-model tree.

The proposed architecture keeps both patterns: a common application for the two
HiBy devices with an extended profile table, and the V1 fork's board profile for
the small panel. Forcing the V1 into the HiBy table would require changing fixed
UI constants (`padding=15`, `top_bar_height=44`, keyboard height 316, gesture
180) that were designed for a 480-pixel-wide panel.

## Integration status

V1 device support is implemented in the shared binary (model row, board profile,
compact fonts, jack quirk), and the V1 asset tree and packer were imported from
the fork. Per-device build targets and CI exist. `make r1` and `make v1` produce
structurally validated images; `make r3proii` is blocked by a mandatory metadata
check in the available stock image. No physical-device validation was performed,
and V1-fork-only features beyond device support were not ported.
