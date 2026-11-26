# Feature inventory — upstream, V1 fork, local

Status key: **V** = verified from source/commands; **I** = inference from source;
**?** = not established. Sources: upstream `Tartarus6/sonix-player` @ `5d8bb17`;
V1 fork `mr-f0xx/sonix-player-tempotec-v1` @ `e962720`; local
`sonix-player` @ `7aa3637f7` plus its uncommitted work. Findings come from the
three investigation reports in this session; see `IMPLEMENTATION_COMPARISON.md`
for the neutral comparison and provenance.

| Feature | Upstream | V1 fork | Local tree | Evidence |
|---|---|---|---|---|
| Shared binary, runtime device profile | V | V (fork replaces with board profile, one model) | V | `sysinfo.c:23-48` (`MODELS` R1/R3PII); V1 `board_profile.c/h` |
| HiBy R1 support (480×800, CS43131, CST8xx, AXP2101) | V | removed (V1-only fork) | V | README:35-49; `assets/R1` |
| HiBy R3 Pro II support (480×720, dual CS43198, GT9xx) | V | removed | V | README:35-49; `assets/R3PII` |
| TempoTec V1 support (240×320, dual CS43131, CST8xx) | absent | V (only model) | absent | V1 `sysinfo.c`; `assets/V1`; `board_profile.*` |
| Firmware packer (ISO 9660 + MD5 chunks) | V, 2 models | V, 1 model + stricter stock verification | V, 2 models | `sonix_firmware_packer.sh:76-79` |
| Per-device build targets (`r1`, `r3proii`, `v1`) | absent | absent (web/emu targets only) | absent | Makefiles: `all host target …`; no device targets |
| GitHub Actions firmware build | absent at HEAD (branch only) | V (`build-tempotec-v1.yml`) | absent at HEAD (branch `github-actions-build`) | `.github/workflows` |
| OTA via GitHub Releases; size check, SHA-256 only when published | V | V (points at V1 repo) | V (`ota-repo: Jepl4r/…` now dead) | `ota.c:264-289,479`; `sysinfo.c:151` |
| MD5 chain + manifest integrity, no signature | V | V | V | packer; ISO contents; `ota.c` (no signature check) |
| Recovery documented (vol+power; `bootmode.sh Recovery`) | V | V (stock firmware backup required) | V | README; `firmware.c:139` |
| AXP2101 cell-charge stop/restore with 5 s retry | absent at HEAD | I: independent V1 implementation | V (3 local commits, tests) | local `7674e85d0`,`b654e440e`,`7cdb63d9a`; V1 `axpcharge.c` |
| Stop-after-current-track playback mode | absent | absent (V: no `STOP_AFTER` symbols) | V (committed) | local `987595ed2` et al. |
| Car mode as phone-identity switching + pairing consent | absent | different design (SBC-only bluealsa restart) | V (committed) | local `1e20ffac6`,`a8a0fcf5f`,`7aa3637f7` |
| A2DP keep-alive player API | absent | absent | V (committed) | local `fd56b3254`; `audio.c:4594+` |
| Browser remembers folder + scroll across restart | absent | V (tracked, tested: `022e7110e`) | **partial**: uncommitted `SNXBRW1` copy + committed branch `feat/browser-state-230` (`SNXBRW2`) | `browserstate.c/h`; V1 `tests/test_browserstate.c` |
| Lock-screen date / rotate row | absent | ? | **uncommitted** (`screensaver.c`, `settings.c`) | matches branch `feat/screensaver-date-271` |
| USB-host screen wake | absent | ? | **uncommitted** (`usb.c:17`) | local diff |
| 48 MiB image gate / `strace` removal | absent | absent | V (local `00b27aab1`) | packer diff |
| V1 web emulator / WASM + screenshot key | absent | V | absent | V1 `tools/v1-emulator`, `web-emulator-pages.yml` |
| Themes, DLNA, lyrics overlay, stats, extra games | absent in upstream HEAD | V (fork-only) | absent | V1 commits listed by archaeology |
| V1 1.3 stock OTA parts bundled + verified | N/A | V | absent | V1 `firmware-input/README.md`; `verify_upt.sh --stock` |
| Built firmware images as GitHub release assets | V (v1.3.1 `r1.upt`, `r3proii.upt`) | V (`v1.upt`, `v1_md5.txt`, 20+ tags) | many test releases on `CookieCaptainD` | GitHub Releases API |
| Known-good *stock* reference images on disk | none in upstream clone | V1 1.3 parts in fork | `r1_original.upt`, copies of `r3proii_original.upt` elsewhere | `find`/`file` |

## Notes on completeness

* The local 16 commits and the uncommitted changes are real work but have **not**
  been integrated into the V1 fork or upstream. Several local features have a
  different V1 design (car mode), so a direct cherry-pick would be wrong.
* The uncommitted browser-state copy is not in the local `Makefile` and has no
  test in the primary tree, so it is classified **partial**, not finished.
* No `.upt` image tracked in any git tree; all firmware artefacts are release
  assets or untracked build outputs. The primary local tree has no
  `r3proii_original.upt`; copies exist in other local worktrees.
* No image in any source has a cryptographic signature. Integrity is MD5 chain
  plus GitHub release SHA-256 only.
