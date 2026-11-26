# Local working tree — initial state and preservation strategy

Status: **verified by direct inspection** on 2026-11-22. Commands and their
output are quoted where they establish a fact. Nothing in this document was
inferred from the project description alone.

## 1. What "the local repository" actually is

The path given as the workspace root, `/home/diogo/Downloads/Hiby`, is **not a
Git repository**. `git status` there fails with `não é repositório git`. It is a
container holding many independent repositories and worktrees.

The primary project repository is:

```
/home/diogo/Downloads/Hiby/sonix-player
```

Its source tree is nested one level down at
`sonix-player/sonix-player/` (the inner directory is not a separate repository;
`git -C sonix-player/sonix-player rev-parse --show-toplevel` returns the outer
repository). That layout is unusual but is the existing project convention and
was preserved.

Other repositories in the container include `sonix-build/sonix`,
`sonix-browser-pr`, `sonix-dac-eq`, `sonix-mseb-clip`, `sonix-player-pr`,
`sonix-scrolling-text`, `sonix-usb-273`, `sonixlink-recovery`, and roughly
thirty worktrees under `worktrees/` and `sonix-player/worktrees/`. These were
observed, not modified. They hold overlapping feature branches and duplicated
firmware reference images; they are part of the local work that must not be
discarded.

## 2. Initial state of the primary repository

| Item | Observed value |
|---|---|
| Branch | `feat/stop-after-current-track` |
| HEAD | `7aa3637f7e7e0ad193994c79c055424a4edf056a` |
| HEAD subject | `feat: restore car pairing, PIN prompts and stable A2DP` (2026-11-18) |
| Remote `fork` | `https://github.com/CookieCaptainD/sonix-player` (fetch + push) |
| Remote `origin` | `https://github.com/Jepl4r/sonix-player` (fetch + push) — **dead**; `git ls-remote` returns `Repository not found` (HTTP 404) |
| Staged changes | 0 |
| Unstaged tracked changes | 11 files |
| Untracked entries | 18 |
| Submodules | none (`.gitmodules` absent) |
| Tags | `1.0.0`, `1.0.1`, `1.1.0`, `1.2.0`, `1.3.0` plus numerous test tags |

`origin/main` is at `fe94fe796` — `Redesigned graphic EQ page, bit-perfect,
settings pages reorganized`. HEAD is **16 commits ahead** of both `origin/main`
and `fork/main` (`git rev-list --count fork/main..HEAD` = 16).

### 2.1 Unstaged tracked changes (`git diff --stat`)

```
 FEATURES.md                                   |   5 +-
 PATCHES.md                                   |  22 ++++
 sonix-player/src/gui/library/browser.c        | 151 ++++++++++++++++++++-
 sonix-player/src/gui/library/browser.h        |  12 +-
 sonix-player/src/gui/settings/screensaver.c   |  60 +++++++++-
 sonix-player/src/gui/settings/screensaver.h   |   4 +
 sonix-player/src/gui/settings/settings.c      |  76 ++++++++++---
 sonix-player/src/gui/shell/powermenu.c        |   2 +
 sonix-player/src/system/device/factoryreset.c |   8 +-
 sonix-player/src/system/device/power.c        |   5 +
 sonix-player/src/system/device/usb.c          |  17 +++
 11 files changed, 336 insertions(+), 26 deletions(-)
```

The `FEATURES.md` / `PATCHES.md` text and the new `browserstate.c/.h` describe a
"remember the folder browser across a restart" feature: an atomic
`device_config.ini.browser` state file, `realpath`-validated card paths,
debounced scroll writes and factory-reset cleanup. This is **uncommitted,
partially integrated work**. It is also present as a committed branch
`feat/browser-state-230` at `417323cf8` in a separate worktree. The two copies
must be compared before deciding which is canonical; no decision is recorded
here.

### 2.2 Untracked entries

```
.firecrawl/
sonix-packer/r1_car_mode.upt
sonix-packer/r1_full_phone.upt
sonix-packer/r1_keepalive.upt
sonix-packer/r1_keepalive_nosbc.upt
sonix-packer/r1_keepalive_playing.upt
sonix-packer/r1_no_aap_probe.upt
sonix-packer/r1_phone_cod.upt
sonix-packer/r1_phone_profiles.upt
sonix-packer/r1_phone_profiles_v2.upt
sonix-packer/r1_pin_sbc_ui.upt
sonix-packer/sonix_player.bak
sonix-player/config.ini
sonix-player/scan_folders.txt
sonix-player/src/gui/library/browserstate.c
sonix-player/src/gui/library/browserstate.h
sonix-player/worktrees/
worktrees/
```

The `*.upt` files are generated firmware images (each roughly 44–50 MiB) and
must be treated as **build artefacts**, not source. `sonix_player.bak` is a
backup binary. `config.ini` / `scan_folders.txt` are runtime configuration with
potentially device-specific or private content; they were not read into any
report and must not be published.

### 2.3 Local commits not present on any remote

`git log --oneline fork/main..HEAD` (identity check with `origin/main..HEAD`
gives the same 16 commits):

```
7aa3637f7 feat: restore car pairing, PIN prompts and stable A2DP
b35c3e3f1 docs: add car mode and PIN screenshots
c43ee4a3e fix: harden car mode switching and finalise image assets
fd56b3254 build: land keep-alive API and drop simulator browser scroll hook
e8eaf756f fix: disable pairing consent by default
6cfc06b77 build: ship phone class main.conf for R1
00b27aab1 build: drop unused strace to keep images under 48 MiB
a8a0fcf5f feat: add car mode toggle and pairing consent sheet
1e20ffac6 feat: add car mode identity switching and authorization prompt
701467e00 feat: add car mode config files and pairing consent strings
7674e85d0 axpcharge: prove the release waits the five-second period
b654e440e axpcharge: retry a failed release on the five-second period
7cdb63d9a axpcharge: stop and restore cell charging on the AXP2101
25b64ba03 playback: remove stop-after settings toggle
4a4431f2b playback: keep stop-after toggle in sync with mode changes
987595ed2 playback: add stop-after-current-track mode
```

These are local work not present on `fork/main` or `origin/main`. Their
provenance relative to the V1 fork is assessed separately in
`LOCAL_V1_GAP_ANALYSIS.md`.

### 2.4 Existing local branches and worktrees

Dozens of local branches exist (e.g. `feat/browser-state-230`,
`feat/avrcp-browsing*`, `fix/r1-v2-ota-compatibility`, `github-actions-build`,
`integration/r1-pr250-259`), many checked out in worktrees under
`/home/diogo/Downloads/Hiby/worktrees/` and
`/home/diogo/Downloads/Hiby/sonix-player/worktrees/`. A complete `git worktree
list` was captured during investigation and is treated as inventory. None were
removed, pruned or checked out.

## 3. Preservation strategy

The following rules were adopted before any change:

* **No destructive Git operation** (`reset --hard`, `clean -fd`, forced
  checkout, stash, amend, rebase or history rewrite) was executed on the user's
  branch or any existing worktree.
* The original working tree at `/home/diogo/Downloads/Hiby/sonix-player` was
  left exactly as found: the 11 modifications and 18 untracked entries remain
  in place.
* No untracked file was deleted, moved or overwritten.
* No push, tag, release or PR was created. No physical device was flashed.
* Upstream and V1 fork were cloned into a separate directory,
  `/tmp/sonix-investigation/`, so investigation could not touch the user's work.

## 4. Isolated integration environment

A dedicated worktree and branch were created from the unmodified HEAD commit:

```
git -C /home/diogo/Downloads/Hiby/sonix-player \
  worktree add -b integration/three-device \
  /home/diogo/Downloads/Hiby/worktrees/integration-three-device HEAD
```

* Branch: `integration/three-device`
* Path: `/home/diogo/Downloads/Hiby/worktrees/integration-three-device`
* Base commit: `7aa3637f7e7e0ad193994c79c055424a4edf056a`

All new documents, tests and code are written to this worktree. Because the
worktree starts from the committed HEAD, the 11 unstaged source changes and the
18 untracked files are **not** automatically present here; they were analysed
in place and any decision to adopt them is recorded explicitly in
`LOCAL_V1_GAP_ANALYSIS.md` and `IMPLEMENTATION_COMPARISON.md`. Adding a worktree
and a branch changes only Git metadata (a new branch ref and worktree
registration) and is reversible without touching the user's files.

## 5. Limits of this report

* The 16 local commits were identified by SHA and subject; their full diffs
  were not all reviewed at the time of writing. Feature-level conclusions are
  recorded only where a diff was actually read.
* File sizes for the untracked `.upt` artefacts are reported from directory
  listings, not from a firmware-format parser. Their structure is assessed in
  the firmware documentation, not here.
* No claim is made here about which device any `.upt` image is safe to flash.
