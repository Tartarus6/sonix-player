# Local improvements absent from the V1 fork

Scope: the 16 local commits on `feat/stop-after-current-track`
(`fe94fe796..7aa3637f7`) and the 11 unstaged tracked files plus untracked
`browserstate.c/.h`. "Absent" was established by content search in the V1 fork
(`e962720`), not merely by commit presence. Where the fork has a different
implementation of the same idea, that is stated.

Decisions use the vocabulary from the task: adopt upstream (A), adopt V1 (B),
adopt local (C), hybrid (D), new (E), preserve (F), exclude (G). These are
**engineering decisions for the proposed integration**, not claims that code has
already been merged.

## 1. Stop-after-current-track playback mode — decision C (adopt local)

Local commits `987595ed2`, `4a4431f2b`, `25b64ba03` add
`PLAYBACK_MODE_STOP_AFTER`, quick-panel control, SonixLink mapping, 11
translations and 4 screenshots. The V1 fork has no `STOP_AFTER` symbols in its
playlist/player/quickpanel/sonixlink sources and no "stop after" string in
`FEATURES.md`. It is a self-contained playback feature with no hardware
dependency. Adopt the local implementation for all three devices; it needs no
V1-specific change. Validation: source inspection only; no runtime test has been
run on this branch.

## 2. Car mode as phone-identity switching — decision D (hybrid, deferred)

Local commits `701467e00`, `1e20ffac6`, `a8a0fcf5f`, `6cfc06b77`,
`e8eaf756f`, `c43ee4a3e`, `7aa3637f7` implement a `bluetooth_car_mode_*` API,
pairing-consent sheet, PIN prompts, and `main.conf` class swapping for R1/R3PII.
The V1 fork has a *different* car mode (`car_mode = auto/on/off`, SBC-only
bluealsa restart) and no HiBy `main.conf` assets. These are not interchangeable.
Do not cherry-pick the local design onto V1; keep V1's design there and the
local design on HiBy devices until a hardware-level comparison is possible. This
is a documented disagreement, not a resolved unification.

## 3. Player-side A2DP keep-alive API — decision C (adopt local, HiBy only)

Local commit `fd56b3254` adds `audio_bt_keepalive_start/stop/active`. V1 has only
the shared bluealsa `--keep-alive` config, no player API. Adopt on HiBy devices;
leave V1 untouched. Risk: Bluetooth behaviour is safety-adjacent (the local
branch is explicitly about restoring stable A2DP), so this needs real-device
validation before any release claim.

## 4. Browser folder/scroll persistence — decision B (adopt V1 version, replace local copy)

Three copies exist: tracked and tested in V1 (`022e7110e`,
`tests/test_browserstate.c`), a committed local branch `feat/browser-state-230`
at `417323cf8` with an `SNXBRW2` migration, and an **untracked** local `SNXBRW1`
copy in the working tree. The local uncommitted copy has no `Makefile` entry and
no test in the primary tree. The V1 version is the most complete verified
implementation, so the local untracked copy should be replaced by the V1 design
rather than promoted. The committed `SNXBRW2` branch must be compared before
final selection; no conclusion is recorded here.

## 5. AXP2101 cell-charge stop/restore with 5 s retry — decision F (preserve both)

Local commits `7cdb63d9a`, `b654e440e`, `7674e85d0` (with tests on the primary
tree) and independent V1 work (`d6f0dbc68`, `45ee6c845`, `b4fd94235`) address the
same PMIC area. V1 has `REG18h[1]` cell-charge control and a 5000 ms reassert but
no `test_axpcharge`. Provenance of each path is unresolved. Keep each device's
current implementation; do not replace working power code without device
evidence. This is the highest-regression-risk area in the whole comparison.

## 6. `strace` removal / 48 MiB image gate — decision C (adopt local build rule)

Local commit `00b27aab1` removes `usr/bin/strace` to keep images under 48 MiB.
V1's packer has no equivalent. This is a build-only change with no runtime
behaviour; adopt it where the image-size budget applies, with the caveat that
V1 images are already ~51 MB and have their own size constraints.

## 7. HiBy `main.conf`/`main.audio.conf` assets — decision G (exclude from V1)

R1/R3PII Bluetooth class assets are meaningless on V1 hardware and its fork has
none. Keep them scoped to HiBy assets only.

## 8. Uncommitted lock-screen date and USB-host wake — decision F (preserve, not integrated)

`FEATURES.md`, `PATCHES.md`, `screensaver.c/h`, `settings.c`, `powermenu.c`,
`power.c`, `factoryreset.c`, `usb.c` contain uncommitted work that matches
branches `feat/screensaver-date-271` and the browser-state branch. It is real but
unfinished and unvalidated here. Preserve in place; do not fold into the
integration commit without a separate review.

## Explicit statement

Every item above is a local improvement absent from the V1 fork. None has been
merged into the V1 fork or upstream by this work. The decisions are proposals
recorded for review; the current integration worktree contains the original
committed HEAD plus documentation and a validator, not these feature merges.
