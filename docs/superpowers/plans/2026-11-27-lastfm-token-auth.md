# Last.fm token sign-in on SonixOS 1.3.3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-enable Last.fm in the HiBy 1.3.3 firmware and sign users in with `auth.getToken` + `auth.getSession` through a QR code, instead of `auth.getMobileSession`.

**Architecture:** Keep `playcount.c` as the single source of playback tracking; keep the Last.fm worker, session file and scrobble queue unchanged. Replace only the login handshake: the worker fetches a token, publishes the authorization URL through `lastfm_status_t`, and converts it to a session key when the GUI says the user pressed Done. The settings screen becomes a QR + Done/Retry page following the existing Tidal device-login pattern.

**Tech Stack:** C (LVGL 8, SDL2 host simulator), pthreads, MD5-signed Last.fm REST calls, Python language-key checker.

**Spec:** `docs/superpowers/specs/2026-11-27-lastfm-token-auth-design.md`

## Global Constraints

- Base is upstream `Tartarus6/sonix-player` tag **v1.3.3**, commit `c54f8e42ea1533ca75e7b970f048ebecba30d98c`. Do not use the TempoTec V1 fork or any of its addon/dynamic-key code.
- Devices: **HiBy R1 and HiBy R3 Pro II only**; language files exist under both `sonix-packer/assets/R1/usr/resource/sonix/language/` and `sonix-packer/assets/R3PII/usr/resource/sonix/language/`.
- Free API methods after 1 Dec 2026: `track.scrobble`, `track.updateNowPlaying`, `track.removeNowPlaying`, `auth.getSession`, `auth.getToken`. `auth.getMobileSession` must be gone from the login path.
- Token: 32-hex, valid **60 minutes**, single use. Errors: 4 invalid token, 14 not authorized, 15 expired.
- `api_key` / `api_secret` are read once in `lastfm_init()` from `streamkeys_lastfm_key()` / `streamkeys_lastfm_secret()`; there is no runtime key reload in this release.
- `lv_conf.h` already has `LV_USE_QRCODE 1`; follow `tidalpage.c:build_login_page()` for QR colours, quiet zone and URL fallback.
- The repo has no `tests/` directory. Verification is: temporary headless harness in `/tmp/opencode` (not committed), `make host`, `python3 scripts/check_languages.py`, and simulator screenshots.
- Do not change `playcount.c`, `listenbrainz*`, the queue file format, or `lastfm.session` format.

## Review Focus

- **Wi-Fi off when sign-in starts:** must show `enable_wi_fi_first` and send no token request — Task 3, simulator check in Task 5.
- **Token expires while the screen is open:** must end with `LASTFM_LOGIN_EXPIRED`, never send `auth.getSession` with a stale token — Task 2 test assertion 4.
- **Done pressed before the phone authorizes (error 14):** must end with `LASTFM_LOGIN_NOT_AUTHORIZED` and leave the user on the screen with a retry hint — Task 2 test assertion 3, Task 3 UI handling.
- **QR generation fails:** the written URL must remain visible and selectable — Task 3 code step and Task 5 screenshot check.
- **User leaves the sign-in screen and comes back:** the token and `auth_url` must still be in `lastfm_status_t` until the deadline — Task 2 test assertion 2.

---

### Task 1: Re-enable Last.fm in the 1.3.3 tree

**Files:**
- Modify: `sonix-player/src/main.c:1964`
- Modify: `sonix-player/src/gui/settings/musicsettings.c:1904-1905`
- Modify: `README.md` ("Streaming keys" section)
- Modify: `sonix-player/streaming-keys.ini.example`

**Interfaces:**
- Consumes: `lastfm_init()` and `lastfmsettings_init(cfg)` already exist, currently commented out.
- Produces: Last.fm reachable from Music settings; `lastfm_init()` runs at startup.

- [ ] **Step 1: Confirm the disabled lines are the only blockers**

Run: `grep -n "lastfm_init" sonix-player/src/main.c && grep -n "lastfmsettings" sonix-player/src/gui/settings/musicsettings.c`
Expected: both occurrences commented out; `playcount_init()` already present and uncommented in `main.c`.

- [ ] **Step 2: Restore init and settings row**

Uncomment exactly `lastfm_init();` in `main.c` and the two lines `lastfmsettings_init(cfg);` / `settingsrow_add(container, "lastfm", NULL, switch_screen_cb, lastfmsettings_screen());` in `musicsettings.c`. Leave the explanatory NOTE/TODO comments in place.

- [ ] **Step 3: Restore key documentation**

In `README.md` "Streaming keys", list Last.fm again with the other services and say the API account comes from last.fm/api. In `streaming-keys.ini.example`, remove the `; only used if Last.fm is enabled in musicsettings.c` comment above `[lastfm]` and keep the empty `api_key` / `api_secret` pair.

- [ ] **Step 4: Verify the build**

Run: `cd sonix-player && make host`
Expected: exits 0, no new warnings about unused `lastfmsettings` functions.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/main.c sonix-player/src/gui/settings/musicsettings.c README.md sonix-player/streaming-keys.ini.example
git commit -m "feat: re-enable Last.fm in 1.3.3"
```

### Task 2: Token-based sign-in in the API layer

**Files:**
- Modify: `sonix-player/src/system/lastfm/lastfm.h`
- Modify: `sonix-player/src/system/lastfm/lastfm.c`
- Create (temporary, not committed): `/tmp/opencode/lastfm_token_test.c`

**Interfaces:**
- Consumes: `call(param_t *, int, char *, size_t, int *, char **)` in `lastfm.c`; `save_session(key, name)`; existing `login_serial` / `login_result` completion mechanism.
- Produces: `void lastfm_begin_login(void)`, `void lastfm_complete_login(void)`; `lastfm_status_t` gains `char auth_url[256]`, `unsigned auth_serial`, `int auth_seconds_left`; `lastfm_login_result_t` gains `LASTFM_LOGIN_EXPIRED`, `LASTFM_LOGIN_NOT_AUTHORIZED` and drops `LASTFM_LOGIN_WRONG_CREDENTIALS`; `lastfm_state_t` gains `LASTFM_STATE_AWAITING_AUTH`.

- [ ] **Step 1: Write the failing headless test**

Create `/tmp/opencode/lastfm_token_test.c`. It must define stubs for `config_path`, `config_get_bool`, `config_set_bool`, `config_save`, `md5_hex`, `wifi_get_status`, `http_request`, `http_last_error`, `storage_sd_root`, `storage_sd_device`, `storage_card_attached`, `storage_sd_writable`, `usb_storage_active`, `streamkeys_lastfm_key` (returns `"testkey123"`), `streamkeys_lastfm_secret` (returns `"testsecret456"`). `#define http_request stub_http_request` before `#include "../src/system/lastfm/lastfm.c"` so the statics (`token_deadline`, `pending_token`) are reachable. Stub `http_request` records the last POST body and returns a configurable XML reply.

Assertions, in this order:
1. `lastfm_begin_login()`; poll `lastfm_get_status()` until `auth_serial` changes; assert `st.auth_url` contains `api_key=testkey123` and `token=`, and the recorded body contains `method=auth.getToken` and `api_sig=`.
2. Call `lastfm_complete_login()` with reply `<lfm status="ok"><session><name>tester</name><key>session123</key></session></lfm>`; poll until `login_serial` changes; assert `login_result == LASTFM_LOGIN_OK`, `st.user == "tester"`, and the session file contains `session123` and `tester`. Leave the token/URL in place and assert `st.auth_url` is still readable before completion is requested.
3. Restart with a fresh token, reply `<lfm status="ok"><error code="14">Not authorized</error></lfm>`; assert `login_result == LASTFM_LOGIN_NOT_AUTHORIZED` and the session file is unchanged.
4. Fresh token, then set `token_deadline = boot_seconds() - 1`; assert `login_result == LASTFM_LOGIN_EXPIRED` and no `auth.getSession` request was recorded.

- [ ] **Step 2: Run the test and confirm it fails**

Run: `gcc -DHOST_BUILD -I sonix-player -o /tmp/opencode/lastfm_token_test /tmp/opencode/lastfm_token_test.c sonix-player/src/system/core/text.c sonix-player/src/system/core/md5.c sonix-player/src/system/core/config.c -lpthread && /tmp/opencode/lastfm_token_test`
Expected: compile error naming `lastfm_begin_login` (function not yet present).

- [ ] **Step 3: Implement the API changes**

In `lastfm.h`: apply the signatures and fields from Interfaces, and update the file comment to describe token sign-in instead of username/password.
In `lastfm.c`: remove `login_user`, `login_password` and the password wipe; add the four worker fields; implement `do_get_token()` and `do_get_session()` with signed `call()` and the error mapping from Global Constraints; replace `lastfm_login()` with the two new public functions; extend `lastfm_get_status()`; clear token state in `lastfm_logout()`; include `begin_requested || complete_requested` in the worker's sleep check so a request always wakes it. Keep `auth_url` built as `https://www.last.fm/api/auth/?api_key=<key>&token=<token>` using the real `api_key`.

- [ ] **Step 4: Run the headless test and confirm it passes**

Run: same command as Step 2.
Expected: `lastfm_token_test: ok`, no failed assertion.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/system/lastfm/lastfm.h sonix-player/src/system/lastfm/lastfm.c
git commit -m "feat(lastfm): sign in with auth.getToken/getSession"
```

### Task 3: QR sign-in screen

**Files:**
- Modify: `sonix-player/src/gui/settings/lastfmsettings.c`
- Modify: `sonix-player/src/gui/settings/lastfmsettings.h` (only if a helper declaration is needed)

**Interfaces:**
- Consumes: `lastfm_begin_login()`, `lastfm_complete_login()`, `lastfm_status_t.auth_url`, `auth_serial`, `auth_seconds_left`; states `LASTFM_STATE_AWAITING_AUTH`, `LASTFM_STATE_SIGNING_IN`; results `LASTFM_LOGIN_EXPIRED`, `LASTFM_LOGIN_NOT_AUTHORIZED`.
- Produces: the new sign-in screen; no new public API.

- [ ] **Step 1: Replace the input widgets**

Remove `user_field`, `password_field`, `password_eye_icon`, `login_keyboard`, `password_eye_cb`, `field_focus_cb`, `make_field()` and the now-unused `keyboard.h` include. In `open_login()`, check Wi-Fi with `wifi_get_status()`; if not connected show `enable_wi_fi_first` and return; otherwise call `lastfm_begin_login()` and `switch_screen(login_screen)`.

- [ ] **Step 2: Build the QR screen in `build_login()`**

Create one centred column like `tidalpage.c:build_login_page()`: hint label (`lastfm_auth_note`), `lv_qrcode` (black on white, white quiet-zone border, size 200 capped to `cfg->screen_width - 2*cfg->padding`), a written-URL label used when `lv_qrcode_update()` fails, a countdown label, a "Done" button calling `lastfm_complete_login()` and a "Retry" button calling `lastfm_begin_login()`. Guard both buttons with `switcher_back_drag_active()`. Do not create a keyboard.

- [ ] **Step 3: Wire the status updates**

In `tick_cb()`, watch `auth_serial`: when it changes, update the QR from `st.auth_url` (or show the URL fallback) and refresh the countdown from `st.auth_seconds_left`. Add `LASTFM_STATE_AWAITING_AUTH` to `refresh()`'s status switch using `lastfm_auth_waiting`. In `login_finished()`, map `OK` to `signed_in` + back, `NO_REPLY`/`REFUSED` to the existing toasts, `EXPIRED` to `lastfm_auth_expired`, and `NOT_AUTHORIZED` to a hint on the screen (stay there, keep the token). Do not cancel the token when the screen is left.

- [ ] **Step 4: Verify the build**

Run: `cd sonix-player && make host`
Expected: exits 0, no warnings from `lastfmsettings.c`.

- [ ] **Step 5: Commit**

```bash
git add sonix-player/src/gui/settings/lastfmsettings.c sonix-player/src/gui/settings/lastfmsettings.h
git commit -m "feat(lastfm): QR sign-in screen"
```

### Task 4: Language strings and docs for both HiBy models

**Files:**
- Modify: `sonix-packer/assets/R1/usr/resource/sonix/language/*.ini`
- Modify: `sonix-packer/assets/R3PII/usr/resource/sonix/language/*.ini`
- Modify: `README.md`, `sonix-player/streaming-keys.ini.example` (follow-up wording from Task 1)

**Interfaces:**
- Consumes: `tr("...")` keys used in Task 3.
- Produces: all language files define the same keys.

- [ ] **Step 1: Add the keys**

In every language file of both trees, inside the `; --- lastfmsettings.c` block, add English-valued fallbacks: `lastfm_auth_note`, `lastfm_auth_waiting`, `lastfm_auth_done`, `lastfm_auth_retry`, `lastfm_auth_expired`, `lastfm_auth_not_authorized`. Remove `lastfm_username`, `lastfm_login_note`, `lastfm_wrong_credentials`. Keep shared keys such as `credentials_required` untouched.

- [ ] **Step 2: Update the wording about sign-in**

In README "Streaming keys" and `streaming-keys.ini.example`, state that sign-in is by scanning a QR on the device and that the key/secret are the user's own.

- [ ] **Step 3: Verify language consistency**

Run: `python3 scripts/check_languages.py`
Expected: exit 0, no missing or stale keys for R1 or R3PII.

- [ ] **Step 4: Commit**

```bash
git add sonix-packer/assets/R1 sonix-packer/assets/R3PII README.md sonix-player/streaming-keys.ini.example
git commit -m "chore(lastfm): language strings and docs for QR sign-in"
```

### Task 5: Simulator run and screenshots

**Files:** none committed; screenshots are written to `Screenshots/` in the simulator's working directory and copied to `/home/diogo/Downloads/Hiby/.hiby-1.3.3/screenshots-lastfm/`.

**Interfaces:**
- Consumes: `sonix_player_host` from `make host`; a `streaming-keys.ini` with a placeholder key (enough to draw the QR; no real authorization is possible without the user's key).
- Produces: PNGs showing the Last.fm settings row, the QR sign-in screen, and the Done/retry behaviour.

- [ ] **Step 1: Prepare the simulator**

Copy `sonix-player/streaming-keys.ini.example` to `sonix-player/streaming-keys.ini`, fill `[lastfm] api_key` / `api_secret` with dummy values, and run `cd sonix-player && make host`. Start `./sonix_player_host` with an X display (or `xvfb-run`) from a working directory whose `Screenshots/` folder exists. `system.screenshots` does not need to be on: the simulator's `/tmp/sonix/shot` trigger calls the same capture path.

- [ ] **Step 2: Capture the screens**

Use the simulator's mouse/keyboard to open Music settings → Last.fm (screenshot A), tap Account to open the sign-in screen and wait for the QR (screenshot B), and press Done without authorizing to capture the not-authorized state (screenshot C). For each: `mkdir -p /tmp/sonix && touch /tmp/sonix/shot`, wait ~1 s, then copy the newest PNG from `Screenshots/` to `.hiby-1.3.3/screenshots-lastfm/`.

- [ ] **Step 3: Report**

Show the PNGs to the human partner and state which assertions from Review Focus were visible, and that the real authorization step needs their own Last.fm key.
