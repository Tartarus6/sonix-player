# Firmware Image Safety Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Submit the existing `fix/image-size-gate` work to `Tartarus6/sonix-player` as a PR that refuses firmware images without a bad-block reserve and documents the brick mechanism.

**Architecture:** Rebase one existing fork commit onto the current `main`. Keep its `image.lock`-based per-model budget, stock-base/kernel locks, module/kernel check and end-to-end verifier; drop the fixed `ROOTFS_MAX`/`KERNEL_MAX` constants added by PR #22 so the lock is the only source of truth. Push to a new fork branch and open the PR.

**Tech Stack:** Bash, git/gh CLI, squashfs/ISO tooling (`7z`, `mksquashfs`, `mkisofs`, `split`, `strings`), `tests/test_verify_upt.sh`.

**Spec:** `docs/superpowers/specs/2026-11-27-firmware-image-safety-gate-design.md`

## Global Constraints

- Base: `Tartarus6/sonix-player` `main` at `c54f8e4` (v1.3.3).
- Source commit: `2132906bef5a79dc019b54fd31a6a6b27a36faec` from `CookieCaptainD/sonix-player` `fix/image-size-gate`.
- Reserve values: 8 rootfs PEBs required (1 MiB), 16 PEBs warn, 4 kernel PEBs required; erase block `0x20000` (128 KiB).
- No `doc/` folder in the PR; the guide stays in `README.md` at the repository root.
- Comments in English, ASCII only. No prebuilt binaries, no translations, no device-side OTA changes.
- PR body follows the scheme of PRs #24/#25: title, `Base:`/`Target repo:`/`Branch:` line, `## What changed`, `## Why`, `## Test plan`, `## Risks and limitations`, `## Checklist`. Use the `stop-slop` skill for the prose.
- Hardware boxes get `N/A` with the reason: no R1 or R3 Pro II is connected.

## Review Focus

- A rootfs with 1 free PEB (issue-147 image) must be refused; 1.3.0 with 19 free PEBs must pass — Task 3's budget test pins both sides.
- A v2.0 stock kernel with v1.6 modules must be refused — Task 3's `BADMOD` test pins the `vermagic` message.
- A corrupt chunk or broken md5 chain must be refused — Task 3 adds a negative case that corrupts one chunk and greps for `md5`.
- PR #22's constants must not survive alongside `image.lock` — Task 2 verifies no `ROOTFS_MAX`/`KERNEL_MAX` and no conflict markers remain.
- The PR must not carry the local design doc or the Last.fm work — Task 2 and Task 5 check `git status` and the diff file list.

---

### Task 1: Isolated worktree from the existing commit

**Files:**
- Worktree: `/home/diogo/Downloads/Hiby/worktrees/image-size-gate`

**Interfaces:**
- Consumes: nothing.
- Produces: clean worktree on `fix/image-size-gate` at `2132906`, with a `fork` remote configured on the shared repo.

- [ ] **Step 1: Fetch the fork branch and the target main**

```bash
git -C /home/diogo/Downloads/Hiby/.hiby-1.3.3 remote add fork https://github.com/CookieCaptainD/sonix-player.git 2>/dev/null || true
git -C /home/diogo/Downloads/Hiby/.hiby-1.3.3 fetch fork fix/image-size-gate
git -C /home/diogo/Downloads/Hiby/.hiby-1.3.3 fetch origin main
```

Expected: both refs resolve; `git rev-parse fork/fix/image-size-gate` prints `2132906bef...`.

- [ ] **Step 2: Create the worktree**

```bash
git -C /home/diogo/Downloads/Hiby/.hiby-1.3.3 worktree add -b fix/image-size-gate-rebased /home/diogo/Downloads/Hiby/worktrees/image-size-gate fork/fix/image-size-gate
```

Expected: worktree created; the original checkout still reports `feat/lastfm-token-auth` and its untracked `docs/` untouched.

- [ ] **Step 3: Confirm the starting state**

```bash
git -C /home/diogo/Downloads/Hiby/worktrees/image-size-gate log -1 --oneline
git -C /home/diogo/Downloads/Hiby/worktrees/image-size-gate status --short
```

Expected: `2132906 packer: refuse images that cannot fit or boot`; no changes.

### Task 2: Rebase onto v1.3.3 and resolve the two conflicts

**Files:**
- Modify: `sonix-packer/sonix_firmware_packer.sh`
- Modify: `README.md`
- Unchanged from the commit: `sonix-packer/image.lock`, `base.lock`, `kernel.lock`, `verify_upt.sh`, `tests/test_verify_upt.sh`

**Interfaces:**
- Consumes: worktree from Task 1.
- Produces: branch `fix/image-size-gate-rebased` with parent `c54f8e4`, no conflict markers, locks and verifier intact.

- [ ] **Step 1: Rebase**

```bash
cd /home/diogo/Downloads/Hiby/worktrees/image-size-gate
git rebase c54f8e4
```

Expected: conflicts in `sonix-packer/sonix_firmware_packer.sh` and `README.md`. Do not abort.

- [ ] **Step 2: Resolve the packer conflict**

Keep the commit's version of the file. In the final file: no `KERNEL_MAX`/`ROOTFS_MAX` definitions and no checks using them; the per-model budget comes from `image_lock_field "$MODEL_DIR" 2/3/4`, with `ROOTFS_RESERVE_PEB=8`, `ROOTFS_WARN_PEB=16`, `KERNEL_RESERVE_PEB=4` and the existing `die`/`warn` messages. Keep the commit's base/kernel lock checks, `vermagic` check and the `verify_upt.sh` call after packing. Preserve any v1.3.3-only packer lines that do not conflict with those blocks.

- [ ] **Step 3: Resolve the README conflict**

Keep the v1.3.3 text and add the commit's `verify_upt.sh`/lock file entries in the packer tree listing and its `### The image is checked` section unchanged. No `doc/` or `docs/` entry in the PR's file list.

- [ ] **Step 4: Finish the rebase and check the result**

```bash
git add sonix-packer/sonix_firmware_packer.sh README.md
git rebase --continue
grep -n "ROOTFS_MAX\|KERNEL_MAX\|<<<<<<<\|>>>>>>>" sonix-packer/sonix_firmware_packer.sh README.md || true
git diff --stat c54f8e4..HEAD
```

Expected: rebase completes; the grep prints nothing; the diff lists exactly `README.md`, the packer, the three locks, `verify_upt.sh` and `tests/test_verify_upt.sh`.

### Task 3: Run the checks

**Files:**
- Test: `sonix-packer/tests/test_verify_upt.sh`

**Interfaces:**
- Consumes: rebased tree from Task 2.
- Produces: passing shell syntax check, verifier tests and an explicit historical-number sanity check.

- [ ] **Step 1: Add the md5-chain negative case to the test**

In `sonix-packer/tests/test_verify_upt.sh`, after the `BADMOD` case, add this case in the test's existing `if ... then echo "FAIL: ..."; exit 1; fi` style:

```bash
mkdir -p "$TMP/broken"
7z x "$TMP/TEST.upt" -o"$TMP/broken" -y >/dev/null
printf 'x' >>"$TMP/broken/ota_v0/rootfs.squashfs.0000."*
mkisofs -o "$TMP/BROKEN.upt" -J -r "$TMP/broken" >/dev/null 2>&1
if "$VERIFY" --model TEST --upt "$TMP/BROKEN.upt" --lock "$TMP/image.lock" >"$TMP/broken.log" 2>&1; then
	echo "FAIL: a broken md5 chain was accepted"
	cat "$TMP/broken.log"
	exit 1
fi
grep -q 'md5' "$TMP/broken.log" || {
	echo "FAIL: the broken chain was not named"
	cat "$TMP/broken.log"
	exit 1
}
```

Expected: the added case fails before the fix existed; with the verifier it prints `verify_upt tests: ok`.

- [ ] **Step 2: Run the syntax check**

```bash
cd /home/diogo/Downloads/Hiby/worktrees/image-size-gate/sonix-packer
bash -n sonix_firmware_packer.sh verify_upt.sh tests/test_verify_upt.sh
```

Expected: no output, exit 0.

- [ ] **Step 3: Run the verifier tests**

```bash
./tests/test_verify_upt.sh
```

Expected: `verify_upt tests: ok`, exit 0. (It skips cleanly if `7z`, `mksquashfs`, `mkisofs`, `split` or `strings` is missing; all are present here.)

- [ ] **Step 4: Check the historical numbers against the lock**

```bash
python3 - <<'PY'
PEB = 0x20000
PART = 0x2d00000
for name, size in [("1.3.0", 44638208), ("issue-147", 46960640)]:
    free = PART // PEB - -(-size // PEB)
    print(name, free, "free PEBs")
PY
```

Expected: `1.3.0 19 free PEBs` (passes the 8-block minimum) and `issue-147 1 free PEBs` (refused).

- [ ] **Step 5: Commit the test addition**

```bash
git add sonix-packer/tests/test_verify_upt.sh
git commit --amend --no-edit
```

Expected: one commit on top of `c54f8e4`; `git log --oneline c54f8e4..HEAD` prints one line.

### Task 4: Push the branch to the fork

**Files:** none.

**Interfaces:**
- Consumes: verified branch from Task 3.
- Produces: remote branch `fix/image-size-gate-v133` on `CookieCaptainD/sonix-player`.

- [ ] **Step 1: Push without rewriting the existing fork branch**

```bash
git -C /home/diogo/Downloads/Hiby/worktrees/image-size-gate push fork HEAD:refs/heads/fix/image-size-gate-v133
```

Expected: push succeeds; `git ls-remote fork refs/heads/fix/image-size-gate-v133` matches local `HEAD`. The old `fix/image-size-gate` ref on the fork is untouched.

### Task 5: Open the PR

**Files:** none.

**Interfaces:**
- Consumes: remote branch from Task 4.
- Produces: open PR at `Tartarus6/sonix-player`, with its number and URL reported to the user.

- [ ] **Step 1: Load the stop-slop skill before writing any prose**

Use the `stop-slop` skill. Draft the body in the PRs #24/#25 scheme: title, `Base: main at c54f8e4... Target repo: Tartarus6/sonix-player. Branch: fix/image-size-gate-v133.`, then `## What changed`, `## Why`, `## Test plan`, `## Risks and limitations`, `## Checklist`. Include the exact commands from Task 3 in the test plan and mark the hardware boxes `N/A` with the reason.

- [ ] **Step 2: Create the PR**

```bash
gh pr create --repo Tartarus6/sonix-player --base main --head CookieCaptainD:fix/image-size-gate-v133 --title "Refuse firmware images without a bad-block reserve" --body-file <draft>
```

Expected: PR created; `gh pr view <number> --repo Tartarus6/sonix-player` shows the body and the file list from Task 2 Step 4.

- [ ] **Step 3: Report**

Print the PR URL, the changed-file list and the verification output to the user. Do not merge.
