# Bluetooth Car Mode and Pairing Consent

## Intent

Give the R1 two Bluetooth identities. Car Mode makes the R1 look like a phone to a hands-free head unit. Audio Mode makes it look like an ordinary audio device. Add a local confirmation before the R1 starts pairing, independent of the active identity.

## Goals

- Persist `bluetooth.car_mode`, default `true`.
- Car Mode on: BlueZ advertises phone Class `0x5A020C`, bluealsa registers HFP AG, obexd serves PBAP/MAP, CIEV indications are sent.
- Car Mode off: BlueZ advertises an audio-device Class (`0x240404`), no HFP AG, no obexd, no CIEV. A2DP Source and AVRCP stay available.
- A toggle in Bluetooth settings changes the key and restarts the Bluetooth stack without losing pairings. Reconnect the last device afterwards.
- Persist `bluetooth.confirm_pairing`, default `true`. Found-device pairing shows a consent sheet; `Device1.Pair()` runs only after OK.
- Defer `RequestAuthorization` to a yes/no UI sheet for incoming pairing. Do not tie this to Car Mode.

## Non-goals

- Forcing a real PIN/passkey when Bluetooth negotiation selects Just Works. BlueZ and the remote IO capability decide the authentication method.
- Adding AVRCP browsing or a music library to the car. Stock BlueZ 5.54 does not implement the Target-side browsing PDUs.
- Removing the working PIN/passkey/confirm/display UI. It stays for methods that BlueZ actually requests.
- Changing pairings, link keys, or the fixed Bluetooth address.

## Constraints

- `/etc` is read-only. Two config files ship in the image; `bluetoothd -f <file>` selects one at launch.
- `org.bluez.Adapter1.Class` is read-only. No D-Bus call changes the major/minor class at runtime.
- Pairings live in `/usr/data/bluetooth`; volumes in `/usr/data/bluealsa`; address in `/usr/data/bt_macaddr.txt`. A daemon restart keeps all three.
- `ensure_phone_profiles()` currently latches `started` before reading config. The latch must move after the check so a later mode change can start obexd.
- `bring_stack_down(false)` keeps hci0 and firmware up; it is the safe restart path.
- UI follows `settingsrow_toggle` and LVGL top-layer sheets used in `btsettings.c`.
- All 12 language files need the new keys or non-English users see raw keys.

## Approaches

### A. Config key, two config files, stack restart (chosen)

Store the mode in config. Launch `bluetoothd` with `-f` pointing to `main.conf` (phone) or `main.audio.conf` (audio device). Gate HFP AG, obexd and CIEV on the same key. A toggle writes the key and posts a job that calls `bring_stack_down(false)`, `bring_stack_up()` and `reconnect_last()`. Live links drop during the restart; pairings survive. This is the only approach that changes the advertised major/minor class.

### B. Toggle profiles only

Restart bluealsa with or without `-p hfp-ag` and start/stop obexd. Smaller change and shorter gap, but the device still advertises major class Phone from `main.conf`. It does not deliver an audio-device identity. Rejected.

### C. Runtime HCI `Write Class of Device`

BlueZ caches major/minor and re-applies them on UUID changes and power cycles. The change would be lost. Rejected.

## Pairing consent design

`found_row_clicked_cb()` builds a confirmation sheet on `lv_layer_top()`. Text names the device and asks the user to confirm pairing. OK calls `bluetooth_pair(mac)` and `pairing_watch_start(mac, name)` together. Cancel leaves the device unpaired and starts no watch. The sheet has no code field. A typed code would not reach BlueZ and would add no MITM protection. Incoming `RequestAuthorization` moves from auto-accept to the existing deferred pairing request UI, with yes/no answers.
