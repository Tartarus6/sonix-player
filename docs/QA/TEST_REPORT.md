# Test report

Distinction used throughout: (1) repository/source inspection, (2) automated
tool execution, (3) simulator capture, (4) package validation, (5) physical
hardware testing. **No physical hardware test was performed.** Environment:
Linux aarch64, 2026-11-22, working directory as stated per command.

## 1. Source and repository inspection (automated, read-only)

| Command | Expected | Actual | Result |
|---|---|---|---|
| `git status --porcelain` in `sonix-player` | capture state | 11 modified tracked, 18 untracked, branch `feat/stop-after-current-track`, HEAD `7aa3637f7` | PASS (observation) |
| `git rev-list --count fork/main..HEAD` | count local commits | 16 | PASS |
| `git ls-remote https://github.com/Jepl4r/sonix-player HEAD` | reachable | `Repository not found` (404) | PASS (defect confirmed) |
| `git ls-remote` for Tartarus6 / mr-f0xx / CookieCaptainD | reachable | HEADs `5d8bb17`, `e962720`, `53bd52af` | PASS |
| GitHub Releases API for the three repos | list assets | Tartarus6 v1.3.1: `r1.upt` 47,736,832 B, `r3proii.upt` 48,046,080 B; V1 fork: `v1.upt` + `v1_md5.txt` on 20+ tags | PASS |
| `git clone --depth 80 …/sonix-player-tempotec-v1` | clone V1 fork | success, HEAD `e96272022`, 219 commits | PASS (first full clone failed with HTTP/2 EOF; shallow retry succeeded) |
| `grep` for `r1|r3proii|v1` in local Makefile | find device targets | no such targets; only `all host target …` | PASS (absence confirmed) |

Three parallel read-only investigations (repository archaeology, firmware/
flashability, UI/build/CI/OTA) were dispatched and their reports are reflected
in `docs/audit/` and `docs/DEVICE_COMPATIBILITY.md`. No agent modified a repo.

## 2. Application build

| Command | Expected | Actual | Result |
|---|---|---|---|
| `make -n host` in integration worktree | dry run | cloned LVGL at parse time; reported missing `opusfile`, `wavpack` | partial |
| `cp <original>/*.tar.* sonix-player/ && make host` | host binary | initial attempt exit 2 (`opusfile.h` missing) |
| build `libogg`, `opus`, `opusfile`, `wavpack` into `/tmp/sonix-host-libs`, then `PKG_CONFIG_PATH=... make host` | host binary | exit 0, aarch64 `sonix_player_host` built | **PASS** |

Target build:

| Command | Expected | Actual | Result |
|---|---|---|---|
| `make target` in `sonix-player/` | MIPS binary | exit 0; `check-abi` PASS; stripped 9.7 MB `sonix_player`; 1952-byte `sonix_launch` | **PASS** |

Per-device firmware builds (root `Makefile`):

| Command | Expected | Actual | Result |
|---|---|---|---|
| `make r1` | validated image | exit 0; `build/firmware/r1/r1.upt` 49,932,288 B; validator STRUCTURAL PASS | **PASS** |
| `make v1` | validated image | exit 0; `build/firmware/v1/v1.upt` 49,539,072 B; validator STRUCTURAL PASS | **PASS** |
| `make r3proii` | validated image | exit 2; image built but validator REJECTED (`required_metadata`, `compatibility_present` FAIL) | **FAIL (correct gate behaviour)** |
| `make all` | three images | stops at the R3 Pro II gate | **FAIL (pending stock input)** |

## 3. Simulator capture

| Command (abbreviated) | Expected | Actual | Result |
|---|---|---|---|
| `bwrap … env SDL_VIDEODRIVER=dummy SONIX_PANEL=480x800 ./sonix_player_host` + `touch /tmp/sonix/shot` | PNG from app screenshot path | PNG written to mapped `/mnt/sd_0/Screenshots`; 480×800, 44,443 B; opened and inspected | PASS for R1 main screen only |
| same with per-device working directory and each device's `system-info.json`, after implementing the V1 profile and rebuilding the host binary | three correct frames | 480×800 (HiBy R1), 480×720 (HiBy R3 Pro II), 240×320 (TempoTec V1) PNGs, all opened and inspected | **PASS** for all three devices |

## 4. Firmware package validation (implemented tool)

Tool: `tools/validate_upt.py`. Exact commands and machine-readable reports are
in `docs/QA/validation/`.

| Command | Expected | Actual | Result |
|---|---|---|---|
| `validate_upt.py sonix-packer/r1.upt --expect-device r1` | structural pass, identity HiBy R1 | `STRUCTURAL PASS`, kernel 6 chunks, rootfs 89 chunks, `observed_device: HiBy R1` | PASS |
| `validate_upt.py sonix-packer/r1.upt --expect-device r3proii` | rejection | `REJECTED`, `wrong_device_rejection=FAIL` | PASS |
| `validate_upt.py sonix-packer/r1.upt --expect-device v1` | rejection | `REJECTED`, `wrong_device_rejection=FAIL` | PASS |
| `head -c 40000000 … > r1-truncated.upt` then validate | rejection | `REJECTED`, `iso_readable=FAIL` | PASS |
| zero one byte at offset 3,000,000 then validate | rejection | `REJECTED`, `rootfs_chain=FAIL` | PASS |
| `validate_upt.py sonix-packer/r1_original.upt --expect-device r1` | stock image integrity | kernel/rootfs integrity PASS; `REJECTED` because stock rootfs has no `system-info.json` | PASS (expected behaviour) |

All six negative/positive cases behaved as specified. These are static and
structural checks only. Two limitations of this evidence are explicit:

* `r1-vs-r3proii.json` and `r1-vs-v1.json` are the **same r1 image** presented
  with the wrong expected device; they do not test real R3 Pro II or V1 images.
  The independent safety reviewer repeated the control and additionally tested
  actual R3 Pro II and stock V1 images, reaching the classifications recorded in
  `FLASHABILITY_MATRIX.md`.
* The images under test were copied from the user's local tree and are **not**
  included in this review branch, so the PASS is not reproducible from the PR
  alone. The validator, reports and the commands are included.

The independent safety reviewer also found that generated R3 Pro II images fail
the mandatory compatibility-metadata check, which changed that device's
classification to REJECTED.

## 5. Physical hardware testing

Not performed. No device was connected, no image was flashed, no bootloader or
recovery action was taken. This is a hard limitation, not an omission.

## 6. Unresolved / not tested

* R3 Pro II: an image is produced by the packer but is correctly REJECTED by
  the gate; V1 `make v1` was built and validated. See §6 of this report.
* Upstream official v1.3.1 `r1.upt`/`r3proii.upt`: not downloaded or inspected.
* Cryptographic signature requirements: unknown.
* OTA update flow: not executed.
* CI workflows: read only; no workflow run was triggered.
* Feature tests for the 16 local commits: not executed on this branch.
* `test_axpcharge`: not run.

## 7. Independent review

Two independent reviewers were dispatched after these results; their findings
are recorded in the final report. Any critical or high-severity finding from
those reviews must be addressed before the final approval gate.
