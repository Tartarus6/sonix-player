# Firmware image safety gate - design (short)

## Intent

Stop contributors building a `.upt` that erases a HiBy R1 or R3 Pro II rootfs
and leaves it unbootable. An image that only just fits the nominal partition
still bricks on the first NAND bad block: `mtdinfo` reports a nominal size, the
OTA updater erases the partition before `nandwrite`, and there is no rollback.

## Goals

- Refuse builds whose rootfs/kernel do not leave a reserve of erase blocks.
- Pin the flash layout, accepted stock bases and kernels so the check is the
  same on every machine.
- Document the limits and the brick mechanism in the repository root.
- Verify the finished image end to end, with a test that needs no device.

## Non-goals / anti-goals

- No device-side OTA scripts (those belong to HiBy).
- No hardware flash test: no R1 or R3 Pro II is connected.
- No new prebuilt binaries, translations or user-visible strings.
- No change to the player source or to unrelated open PRs (#24, #25).

## Constraints

- Target: `Tartarus6/sonix-player`, from the `CookieCaptainD` fork.
- Base: current `main` at `c54f8e4` (v1.3.3), which already has PR #22's
  simple `ROOTFS_MAX`/`KERNEL_MAX` gate with **no** bad-block reserve.
- PR template: guide stays in the repository root (no `doc/` folder in the
  PR); comments in English, ASCII only; rebased on current `main`.

## Approaches

1. Fresh minimal PR: subtract four erase blocks from `ROOTFS_MAX` and add a
   short root-level note. Small, but no image verification, no tests and no
   pinning of stock bases or kernels.
2. Reuse the existing fork branch `fix/image-size-gate` (commit `2132906`):
   one commit with `image.lock`, `base.lock`, `kernel.lock`, a hardened packer,
   `verify_upt.sh`, its test and a README section. **Chosen.** It is already
   written and reviewed in earlier sessions, covers both models, and the only
   work is a rebase onto v1.3.3 plus conflict resolution with PR #22.
3. Submit the branch as-is without rebasing. Rejected: it predates PR #22 and
   would collide with the gate now in `main`.

## Decisions (builder's calls, not re-opened)

- Work in an isolated git worktree from the fork branch; leave the existing
  checkout on `feat/lastfm-token-auth` and its untracked files untouched.
- On rebase, keep the `image.lock`-based per-model budget and drop the fixed
  `ROOTFS_MAX`/`KERNEL_MAX` constants: the lock is the single source of truth.
- Keep the reserve from the existing branch: 8 rootfs PEBs (1 MiB) required,
  16 PEBs warn, 4 kernel PEBs required. This rejects the issue-147 test image
  (1 free PEB) and accepts the 1.3.0 release (19 free PEBs).
- The design doc stays local under `docs/superpowers/specs/`; it is not part
  of the PR, which keeps the repository's "no doc/ folder" rule.
- Verification: `bash -n` on both shell scripts and `tests/test_verify_upt.sh`;
  hardware boxes in the PR template get `N/A` with the reason.

## Left to the builder

- Fetch the fork branch, rebase onto `c54f8e4`, resolve conflicts in
  `sonix_firmware_packer.sh` and `README.md`, run the checks, push and open the
  PR with the repository template.
