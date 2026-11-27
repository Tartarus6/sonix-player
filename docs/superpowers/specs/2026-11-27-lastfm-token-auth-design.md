# Re-enable Last.fm with token sign-in on HiBy SonixOS 1.3.3

Date: 2026-11-27
Base: `Tartarus6/sonix-player` tag **v1.3.3**, commit `c54f8e42ea1533ca75e7b970f048ebecba30d98c`
Devices: **HiBy R1 and HiBy R3 Pro II only** (assets `sonix-packer/assets/R1` and `R3PII`).
No TempoTec V1 scope, no V1 fork, no V1 addon/streamkeys reload code.

## Intent and why

v1.3.3 removed Last.fm "for now" because the stock image has no API key. The
code is still in the tree; only the init call and the settings row were
commented out. The removal note says it is "easily undoable" for a fork.

At the same time, Last.fm's 2026-11-26 policy update lists the methods that
keep working without Pro from 1 Dec:

> `track.scrobble`, `track.updateNowPlaying`, `track.removeNowPlaying`,
> `auth.getSession`, `auth.getToken`

`auth.getMobileSession` is **not** on that list. So re-enabling the old
username/password sign-in would leave a key that works for scrobbles but not
for login. This change re-enables the module **and** moves sign-in to the
desktop token flow, with a QR code because the player has no browser.
Sources:
- https://support.last.fm/t/api-update-pro-will-not-be-required-to-use-scrobbling-methods/121188
- https://www.last.fm/api/authspec
- https://www.last.fm/api/show/auth.getToken
- https://www.last.fm/api/show/auth.getSession

## Goals

- Last.fm present again in Music settings and working on R1 and R3 Pro II.
- Obtain a session key via `auth.getToken` + `auth.getSession`; no password is
  typed or stored on the device, and no callback URL is configured.
- The user authorizes on a phone by scanning a QR (or opening the written URL),
  then presses "Done" on the player; the player calls `auth.getSession`.
- Leave scrobbling itself untouched: `playcount.c` tracking, queue in
  `.local/lastfm.queue`, batching, session file, factory reset.

## Non-goals

- No embedded/shared API key and no server relay. The user supplies `api_key`
  and `api_secret` in `streaming-keys.ini`, which is sealed into
  `streaming-keys.bin` by the existing packer flow (README "Streaming keys").
  Creating a new key after 1 Dec requires Pro; a key created before then
  keeps scrobbling when Pro lapses.
- No ListenBrainz changes.
- No new test harness. This repo has no `tests/` directory; verification is
  the host build, the language checker, and a simulator run.
- No support for the TempoTec V1 fork or its dynamic key reload.

## Constraints

- `auth.getToken`: `api_key` + `api_sig`; returns a 32-hex token, valid
  60 minutes, single use, bound to the API account.
- `auth.getSession`: `api_key` + `token` + `api_sig`; returns `<name>` and
  `<key>`. Error 4 = bad token, 14 = not authorized yet, 15 = expired.
- Authorization URL: `https://www.last.fm/api/auth/?api_key=KEY&token=TOKEN`.
  The callback URL field on the Last.fm app is irrelevant to this flow.
- Session keys are bound to the API key that created them. In 1.3.3
  `api_key`/`api_secret` are read once in `lastfm_init()` and never change at
  runtime, so there is no key-swap case to handle here.
- `call()` already sorts params, appends the secret, builds `api_sig` and POSTs
  form-urlencoded to `API_URL`. Reuse it; confirm during implementation that
  Last.fm accepts POST for these two auth methods, falling back to GET only if
  the endpoint requires it.
- `lv_conf.h` has `LV_USE_QRCODE 1`. The Tidal device-login page in 1.3.3 is
  the UI pattern to follow: fixed black/white QR, white quiet-zone border,
  written URL as fallback when QR generation fails.

## Approaches considered

1. **Token flow + QR + Done button (chosen).** Both auth methods are explicitly
   free after 1 Dec. No password on the device. The QR/polling UI already
   exists for Tidal. Cost: the user scans, then presses one button; the token
   has a 60-minute deadline.
2. **Paste a session key from another client.** Small change, but a session key
   is tied to the API key and app that produced it, so it will not work with
   our key. Rejected.
3. **Relay through a Last.fm-compatible endpoint.** No auth change in the
   player, but needs a server, which is wrong for a standalone DAP. Users who
   want a different service already have ListenBrainz in 1.3.3. Rejected.

## Design

### Re-enable the module (1.3.3 specific)

- `sonix-player/src/main.c:1964`: restore `lastfm_init();`.
- `sonix-player/src/gui/settings/musicsettings.c:1904-1905`: restore
  `lastfmsettings_init(cfg);` and the `lastfm` settings row.
- `README.md` "Streaming keys" and `sonix-player/streaming-keys.ini.example`:
  put Last.fm back in the list of services that need keys; drop the note that
  says the row is disabled in `musicsettings.c`.
- No change to `playcount.c`: it already calls `lastfm_note_play()` while
  `lastfm_active()`, and `playcount_init()` runs independently of this page.

### API layer — `src/system/lastfm/lastfm.h`, `lastfm.c`

- Replace `lastfm_login(user, password)` with:
  - `void lastfm_begin_login(void)` — worker calls `auth.getToken`.
  - `void lastfm_complete_login(void)` — worker calls `auth.getSession` with
    the token held by the module.
  Keep `lastfm_init()`, `lastfm_set_enabled()`, `lastfm_logout()`,
  `lastfm_active()`, `lastfm_note_play()`, `lastfm_network_wanted()`.
  The only caller of `lastfm_login()` is `lastfmsettings.c`.
- Remove `login_user[128]` / `login_password[256]` and the password wipe; add
  worker-side `begin_requested`, `complete_requested`, `pending_token[64]`,
  `token_deadline` (CLOCK_BOOTTIME seconds).
- Extend `lastfm_status_t` with `char auth_url[256]`, `unsigned auth_serial`
  (changes when a token arrives or the attempt ends) and
  `int auth_seconds_left` (derived from `token_deadline`).
- Extend `lastfm_login_result_t` with `LASTFM_LOGIN_EXPIRED` and
  `LASTFM_LOGIN_NOT_AUTHORIZED`. Drop `LASTFM_LOGIN_WRONG_CREDENTIALS`.
  Keep `OK`, `NO_REPLY`, `REFUSED`.
- Add `LASTFM_STATE_AWAITING_AUTH` to `lastfm_state_t`: keys present, token
  live, waiting for the user. `LASTFM_STATE_SIGNING_IN` stays for the two
  network calls. `lastfm_get_status()` fills the new fields; the old state
  order and `detail` are unchanged.
- `do_get_token()`: signed `auth.getToken` via `call()`, parse `<token>`, build
  `auth_url`, set deadline (`now + 3600`), publish `auth_serial`, signal the
  GUI. On failure end the attempt with `NO_REPLY`/`REFUSED`.
- `do_get_session()`: require a live token and `now < token_deadline`, else end
  with `LASTFM_LOGIN_EXPIRED`. Signed `auth.getSession`, parse `<key>`/
  `<name>`, `save_session()`, update `session_key`/`user`, clear
  `session_expired`/`refused`/`detail`, reset `session_refusals`, clear the
  token, end with `OK`. Map error 14 to `NOT_AUTHORIZED`, 4/15 to `EXPIRED`.
- Both handlers use the existing `login_serial` / `login_result` completion
  mechanism, so the GUI keeps one polling path.
- `lastfm_logout()` also clears `pending_token` and `auth_url`.
- The worker's sleep condition must include `begin_requested` and
  `complete_requested` so a sign-in is never left parked in `cond_wait`.

### GUI — `src/gui/settings/lastfmsettings.c`

- Remove `user_field`, `password_field`, `password_eye_icon`,
  `login_keyboard`, `password_eye_cb`, `field_focus_cb`, `make_field()` and
  the password/eye/keyboard includes where no longer needed.
- Rebuild `build_login()` as one centred column in the style of
  `tidalpage.c:build_login_page()`:
  - hint label (`lastfm_auth_note`);
  - `lv_qrcode` (black/white, quiet zone, size chosen from `cfg->screen_width`,
    ~200 on R3 Pro II and smaller on R1), updated from `st.auth_url` when
    `auth_serial` changes; hidden with the written URL shown if generation
    fails;
  - countdown / waiting label from `st.auth_seconds_left`
    (`lastfm_auth_waiting`, `lastfm_auth_expired`);
  - "Done" button → `lastfm_complete_login()`, guarded with
    `switcher_back_drag_active()` like other buttons;
  - "Retry" button → `lastfm_begin_login()` again.
- `open_login()` checks Wi-Fi (`enable_wi_fi_first`), starts
  `lastfm_begin_login()` and switches to the screen; the QR appears when the
  token arrives, not synchronously.
- `enabled_cb()` / `account_cb()` keep their behaviour and open the new screen.
  `login_finished()` handles `OK`, `NO_REPLY`, `REFUSED`, `EXPIRED` and
  `NOT_AUTHORIZED`; the last one leaves the user on the screen with a hint
  instead of a toast-only error.
- Leave the screen without cancelling the token: the module keeps it for the
  full 60 minutes, so the user can come back and still scan it.
- `tick_cb()` keeps checking `login_serial` and refreshing the page; add the
  `auth_serial` watch and countdown refresh while the login screen is active.

### Language — both device trees

Files: `sonix-packer/assets/{R1,R3PII}/usr/resource/sonix/language/*.ini`.
Add to the `lastfmsettings.c` block in **every** language file in both trees
(English text as fallback where no translation exists): `lastfm_auth_note`,
`lastfm_auth_waiting`, `lastfm_auth_done`, `lastfm_auth_retry`,
`lastfm_auth_expired`, `lastfm_auth_not_authorized`.
Remove `lastfm_username`, `lastfm_login_note`, `lastfm_wrong_credentials` from
English and all translations. Do **not** remove shared keys such as
`credentials_required`. `scripts/check_languages.py` must pass for both trees.

### Documentation

Update the two places that say Last.fm is off: README "Streaming keys" and
`streaming-keys.ini.example`. Say that sign-in is by QR on the device and that
the user's own key is required.

## Verification

- `cd sonix-player && make host` — host/simulator build must compile cleanly.
- `python3 scripts/check_languages.py` — all keys in sync for R1 and R3PII.
- Run the host simulator with a `streaming-keys.ini` containing a real key,
  open Music settings → Last.fm, confirm the QR is drawn and the written URL
  fallback is present; authorize on a phone, press Done, confirm the username
  appears and scrobbles are sent (check the host log).
- With no keys, confirm the module reports unavailable and the row is disabled,
  as before.

## Risks

- POST acceptance for `auth.getToken` / `auth.getSession` needs one live check.
- Token deadline is local (CLOCK_BOOTTIME); no dependency on the device clock.
- QR legibility on the smaller R1 screen — keep the Tidal quiet-zone treatment
  and always provide the URL fallback.
- The user's key must exist before 1 Dec or be created with Pro; out of scope
  to solve in firmware.

## Left to the builder

Approve this design, then write the implementation plan (writing-plans) and
execute it in this order: re-enable the module, API layer, GUI, language/docs,
verification.
