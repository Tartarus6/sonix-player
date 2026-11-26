# Neutral implementation comparison

Purpose: compare competing implementations without source bias. Alternatives are
first described as **Implementation A / B / C**; origins are revealed only in the
decision line. Criteria were functional correctness, maintainability,
reliability, hardware fit, regression risk and test coverage. No numerical score
is used — the differences are qualitative and a score would imply false
precision. Independent assessments came from the three investigation streams
(archaeology, firmware/safety, UI/build/OTA) recorded in this session.

## 1. Device profile / resolution selection

- **A:** one runtime `MODELS[]` table keyed by `device-name` in `system-info.json`,
  plus a framebuffer-size fallback and a 320×480 floor (`sysinfo.c:23-48`,
  `main.c:1099-1132`). Fixed `padding=15`, `top_bar_height=44` for both panels.
- **B:** a dedicated `board_profile` with 240×320, `padding=6`, status bar 24,
  compact fonts and `bp_pick()` always choosing the compact variant
  (`board_profile.c/h`, V1 `main.c:1073-1117`).
- **C:** no profile — hard-coded single panel.

Assessment: A has served two devices and is well understood; it is not truly
device-parameterised. B is genuinely adapted, not scaled — font rasters are
remapped globally and specific screens use different tile counts — but it is
single-device and removes the other models. A third model can be added to A's
table, but the fixed 44/15 constants must be reviewed for the small panel.

**Decision: hybrid (D)** — retain A for HiBy devices, use B's pattern for V1.
Origin: A = upstream/local; B = V1 fork. Needed modification: extract the
fixed 44/15 constants into profile values before sharing code. **Implemented in
this branch**: `src/gui/board_profile.{c,h}` supplies the profile, `sysinfo.c`
selects it at runtime, and `main.c`/`fonts.c` consume it. Only padding, status
bar height, screen size and compact fonts are wired; the fork's radius and
header-button helpers were not ported.

## 2. Firmware packaging

- **A:** two-model ISO packer with a `MODELS` table, `device-name` gate, stock
  image as base, LZO squashfs, 512 KiB MD5 chunks, `mkisofs -J -r`
  (`sonix_firmware_packer.sh`).
- **B:** single-model V1 packer with stock-package byte-equality verification,
  `base_build_version` rewrite and pre-write compatibility MD5 re-check
  (`verify_upt.sh --stock`); kernel is never replaced.
- **C:** no packaging.

Assessment: both produce the same ISO/OTA skeleton; B adds stronger stock
preservation checks appropriate to a device whose stock kernel is required. A's
cross-device name gate is the key safety property and is present in both.
Neither signs images.

**Decision: preserve both (F)** with A for HiBy and B for V1. A shared packer
would have to encode two different kernel policies; the safety gain does not
justify the regression risk. Origin: A = upstream/local; B = V1 fork.

## 3. OTA selection and integrity

- **A:** `ota.c` selects `<stem>.upt` from the newest 20 GitHub releases, checks
  asset size and verifies the `sha256:` digest only when published
  (`ota.c:479`), writes `.part`, then arms recovery and reboots.
  Stem comes from `system-info.json` or config; no stem means no OTA.
- **B:** identical mechanism pointed at the V1 repo.
- **C:** none.

Assessment: the mechanism is sound for integrity but depends on repository
owner/name, asset filename and release tags. The local `ota-repo` points to a
dead repository (`Jepl4r`), which is a real migration defect. No signature
verification exists anywhere.

**Decision: preserve A/B (F), document migration (`MIGRATION.md`)**. Changing
the canonical repo without updating `system-info.json` would silently disable
OTA. Origin: A = upstream/local; B = V1 fork.

## 4. AXP2101 charging

- **A:** local commits with `test_axpcharge`, five-second retry and explicit
  stop/restore of cell charging.
- **B:** V1 independent implementation with `REG18h[1]`, 5000 ms reassert, no
  test in the fork.
- **C:** upstream HEAD has neither.

Assessment: both address the same hardware; provenance and functional
equivalence are unresolved. Power management is high regression risk.

**Decision: preserve (F)** per device; no replacement. Origin: A = local; B =
V1 fork.

## 5. Browser state

- **A:** V1 tracked implementation with `tests/test_browserstate.c`.
- **B:** local committed branch `feat/browser-state-230` with `SNXBRW2` migration.
- **C:** local untracked `SNXBRW1` copy, no Makefile entry, no test in primary tree.

Assessment: A has the strongest verification evidence in its tree; C is
incomplete. B is unreviewed here.

**Decision: adopt A (B, V1 origin), replace C.** Origin revealed: A = V1 fork;
B/C = local. Justification: test coverage and tracked status.

## 6. Car mode

- **A:** local phone-identity switching, pairing consent, PIN prompts, class
  swap, R1/R3PII assets.
- **B:** V1 `car_mode = auto/on/off`, SBC-only bluealsa restart.
- **C:** none.

Assessment: these differ in mechanism, not just configuration. A depends on
HiBy Bluetooth class files absent on V1; B depends on V1's bluealsa setup. A
unified implementation would be a new design requiring hardware validation.

**Decision: hybrid (D), deferred** — keep per-device implementations; do not
force common code. Origin: A = local; B = V1 fork.

## 7. Validation tooling

- **A:** no automated structural validator in any source.
- **B:** new `tools/validate_upt.py` in this worktree: ISO parse, metadata,
  chunk chain, manifest, whole-file MD5, rootfs device identity, wrong-device
  and corruption rejection.
- **C:** manual `verify_upt.sh` in the V1 fork (stock byte equality).

Assessment: B is independent of the build script and has been executed here with
real images. It establishes static/structural facts only, not flashability.

**Decision: develop new (E)** — keep B alongside C; both are complementary.

## Agent agreement and disagreement

All three investigation streams agreed that: (1) no image is signed; (2) device
identity is readable in the generated rootfs and is the strongest available
cross-device guard; (3) V1's UI adaptation is genuine, not a scale; (4) per-device
build targets do not exist. Disagreement remains on AXP charging equivalence and
on whether the local car-mode design can be shared with V1; both are recorded as
unresolved and block unification of those features.
