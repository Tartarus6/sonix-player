# Review of the nine prepared PR features

Source: the nine PR bodies in `/home/diogo/Downloads/Hiby/pr-bodies/` and their
local branches in the primary repository. Status verified 2026-11-22 with
read-only git/merge analysis. "In V1 fork?" was checked by content search in
`mr-f0xx/sonix-player-tempotec-v1` @ `e962720`, not by name guessing.
"Merge" used `git merge-tree --write-tree <integration> <branch>` — no user
branch was modified by that check.

| PR | Feature | In V1 fork? | Merge into integration | Decision |
|---|---|---|---|---|
| item-1 | Firmware model-mismatch check before flashing + SHA logging | **No** (`firmware_update_model_check`, `OTA_MODEL_MISMATCH` absent) | Conflicts: our CI workflow and `sonix-player/Makefile` | Feasible with small manual resolution; safety-relevant, good candidate |
| item-2 | WavPack DSD, RF64 and Wave64 playback | Partly: RF64/Wave64 are read by libsndfile in `sndfile.c`; WavPack DSD uses `OPEN_DSD_AS_PCM` (decimated), native DSD absent | **Clean** | **Implemented and tested** (merge `77862d1e`) |
| item-3 | Equal-power crossfade with settings | **Yes** — full `src/system/audio/crossfade.{c,h}` plus settings in `musicsettings.c` and `device_state` sync | Conflicts: CI workflow + 24 language files | Prefer the V1 fork implementation (technical neutrality). Not merged from PR; port from fork if desired |
| item-5-app | SonixLink Android thumbnail cache | Not applicable (different repository, Kotlin) | N/A | Out of scope for this firmware codebase |
| item-5-player | MusicBrainz cover fetch from Now Playing | **No** (`coverfetch` absent) | Conflict: `sonix-player/Makefile` | Feasible; network feature depends on Wi-Fi/keys and needs device validation |
| item-6-fw | SonixLink upload/fs/playlist routes + PIN | **No** (`/api/upload`, `X-Sonix-Pin` absent) | Conflicts: CI workflow + language files | Feasible but must be paired with the Android app PR; security-sensitive (path confinement, PIN) |
| item-6 | SonixLink Android Transfer tab + PIN | Not applicable (different repository, Kotlin) | N/A | Out of scope; depends on item-6-fw |
| item-7 | Dim screen before automatic screen-off | **No** (`dimlogic`, `dim_before_timeout` absent) | Conflict: CI workflow only | Feasible; low-risk power/UI feature, good next candidate |
| item-8 | PIN-protected browser remote | **No** (`browserremote` absent) | Conflicts: CI workflow + language files | Feasible but adds an HTTP server and PIN sessions; higher security review burden |

## Implementation performed in this work

`pr/item-2-audio-formats` was merged (`git merge --no-ff`, commit `77862d1e`)
into the integration branch. Verification actually executed:

```
make target                     -> exit 0, check-abi PASS, stripped binary built
PKG_CONFIG_PATH=/tmp/sonix-host-libs/lib/pkgconfig sh tests/run.sh -> exit 0
```

All test groups passed: `dst_decoder_test`, five `sacd_iso_test` modes,
`dsd_sacd_test`, `dsd_regression_test`, `wavpack_dsd_test`,
`metadata_sacd_test`, `library_sacd_test`, `wavparse_test` (including RF64 and
Wave64 fixtures). This adds native WavPack DSD and robust WAV header parsing to
all three devices; it is a genuine improvement over the V1 fork's
`OPEN_DSD_AS_PCM` path.

## Why the others were not merged in this pass

* **item-3**: the best implementation is already in the V1 fork; adopting the
  PR version would duplicate code and create a conflicting settings model.
  Technical neutrality says use the fork's version if crossfade is wanted.
* **item-1, item-5-player, item-6-fw, item-7, item-8**: all are feasible, but
  each needs manual conflict resolution (CI workflow and/or 24 translated INI
  files) plus feature-specific tests and V1 asset translations. Merging five
  untested features at once would violate the "preserve working functionality"
  principle. They are candidates for staged follow-up work, with item-7
  (dimming) and item-1 (firmware safety) the lowest-risk starting points.
* **item-5-app / item-6**: belong to the separate SonixLink Android repository.

No PR was opened or pushed; all work is local to `integration/three-device`.
