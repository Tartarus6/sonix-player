# Bluetooth Car Mode and Pairing Consent Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a persistent Car Mode toggle that changes the advertised Bluetooth identity and a pairing consent gate independent of that mode.

**Architecture:** The Bluetooth worker owns `g_car_mode`. A config write posts `JOB_CAR_MODE`; the worker tears down and brings up the stack with a selected `main.conf`, then reconnects the last device. The GUI owns a toggle and a top-layer consent sheet. BlueZ agents stay `KeyboardDisplay`; `RequestAuthorization` becomes a deferred yes/no request.

**Tech Stack:** C, LVGL, BlueZ D-Bus, BlueALSA, obexd, project config and job queue.

**Spec:** `docs/superpowers/specs/2026-11-18-bt-car-mode-and-pairing-consent-design.md`

## Global Constraints

- `bluetooth.car_mode` default `true`; `bluetooth.confirm_pairing` default `true`.
- Phone Class `0x5A020C`; audio Class `0x240404`.
- Pairings, volumes and BT address survive a stack restart. Never remove pairing state.
- `bluetoothd` accepts `-f <config file>`. Rootfs `/etc` stays read-only.
- UI keys `bt_car_mode`, `bt_pairing_consent`, `bt_pairing_consent_note`, `bt_pairing_consent_confirm` in every R1 and R3PII language file.
- Build targets: host and target. Image from `sonix_firmware_packer.sh`.

## Review Focus

- Audio Mode must not leave HFP AG, obexd or CIEV running.
- Car Mode toggle must not delete pairings or change the fixed MAC.
- Stack restart must finish with `reconnect_last()`; a live A2DP link may drop.
- Toggling mode twice must not leak daemons or leave the second `main.conf` unselected.
- Cancel on the consent sheet must not call `Device1.Pair()` or start the pairing watch.
- Incoming `RequestAuthorization` must not auto-accept when consent is enabled.

---

### Task 1: Config files and translations

**Files:**
- Modify: `sonix-packer/assets/R1/etc/bluetooth/main.conf` (already `Class = 0x5A020C`)
- Create: `sonix-packer/assets/R1/etc/bluetooth/main.audio.conf`
- Modify: all `sonix-packer/assets/R{1,R3PII}/usr/resource/sonix/language/*.ini`

**Interfaces:**
- Produces: two config files selectable by path; translation keys used by Task 3.

- [ ] **Step 1: Create `main.audio.conf` as a copy of `main.conf` with `Class = 0x240404`**
- [ ] **Step 2: Add the four new keys to every language file; English values: "Car Mode", "Confirm pairing", "Pair with %s?", "Pair"**
- [ ] **Step 3: Verify with `grep -L bt_car_mode` over both language directories; expect no output**
- [ ] **Step 4: Commit**

### Task 2: Bluetooth core identity switching

**Files:**
- Modify: `sonix-player/src/system/bluetooth/bluetooth.c`
- Modify: `sonix-player/src/system/bluetooth/bluetooth.h`
- Modify: `sonix-player/src/system/bluetooth/btstack.c`

**Interfaces:**
- Consumes: `bring_stack_down(bool)`, `bring_stack_up()`, `reconnect_last()`, `restart_bluealsa()`, `spawn_daemon()`.
- Produces: `bool bluetooth_car_mode_get(void)`, `void bluetooth_car_mode_set(bool)`, `bool bluetooth_car_mode_restarting(void)`; `BTSTACK_PAIR_AUTHORIZE` request type.

- [ ] **Step 1: Add config getters for `car_mode` and `confirm_pairing`; set `g_car_mode` during worker init**
- [ ] **Step 2: Gate `hfp_ag`, `ensure_phone_profiles()` and `start_hfp_ciev()` on `g_car_mode`; move the obexd latch after the config check; stop obexd in `bring_stack_down()`**
- [ ] **Step 3: Add `-f <path>` to `bluetoothd` argv; path comes from `g_car_mode`**
- [ ] **Step 4: Add `JOB_CAR_MODE`; handler sets busy, calls `bring_stack_down(false)`, `bring_stack_up()`, `reconnect_last()`, clears busy; dedupe the job in `queue_has()`**
- [ ] **Step 5: Add a `BTSTACK_PAIR_AUTHORIZE` state and defer `RequestAuthorization` through the existing pair request path; `value == "yes"` accepts**
- [ ] **Step 6: Build host and target; expect no implicit-declaration or link errors**
- [ ] **Step 7: Commit**

### Task 3: GUI toggle and pairing consent sheet

**Files:**
- Modify: `sonix-player/src/gui/bluetooth/btsettings.c`
- Modify: `sonix-player/src/gui/bluetooth/btstack.h` (pair request enum consumer)

**Interfaces:**
- Consumes: `bluetooth_car_mode_get/set`, `bluetooth_car_mode_restarting`, `bluetooth_pair()`, `pairing_watch_start()`.
- Produces: one settings row; one consent sheet with OK/Cancel.

- [ ] **Step 1: Add `bt_car_mode` toggle after the Bluetooth switch in `btsettings_init()`; callback calls `bluetooth_car_mode_set(on)`; `poll_cb` mirrors the switch state**
- [ ] **Step 2: Add a top-layer consent sheet builder following the PIN sheet pattern; title from `bt_pairing_consent`, message `bt_pairing_consent_note` with device name, buttons `bt_pairing_consent_confirm` and `cancel`**
- [ ] **Step 3: In `found_row_clicked_cb()`, when `confirm_pairing` is enabled, show the sheet and move `bluetooth_pair()` plus `pairing_watch_start()` into its OK callback; Cancel runs neither**
- [ ] **Step 4: Render mode 4 of the deferred pair request sheet as a yes/no question for incoming authorization**
- [ ] **Step 5: Build host and target; launch host with `SONIX_MOCK_BT=1` and capture the Bluetooth page and the consent sheet**
- [ ] **Step 6: Commit**

### Task 4: Verification and image

**Files:**
- Generate: `sonix-packer/r1_car_mode.upt`

**Interfaces:**
- Consumes: the target binary from Task 3.

- [ ] **Step 1: Run `make -j4 host` and `make -j4 sonix_player`; both complete**
- [ ] **Step 2: Run the packer with `fakebin` on PATH; verify the image contains `main.audio.conf`, `Class = 0x240404` inside it, and the new binary strings**
- [ ] **Step 3: Copy the image to `sonix-packer/r1_car_mode.upt`; record build version and md5**
- [ ] **Step 4: Commit generated image if the project tracks artifacts; otherwise leave it untracked and report the path**
