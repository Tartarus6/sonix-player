# AVRCP target browsing design

Date: 2026-11-27
Base: Tartarus6/sonix-player v1.3.3, commit `c54f8e4`

## Intent

Sonix Player answers AVRCP transport commands and publishes track metadata. A car head unit cannot browse the library. This work adds the AVRCP Target browsing role so a head unit can read `Artists`, `Albums` and `Tracks` folders, move back to the root, and start a track from the list.

## Scope

- Start browsing on. A file at `/usr/data/disable_bt_browse` selects the stock daemon on the next Bluetooth start. No settings UI exists.
- Keep the v1.3.3 metadata and transport path in `btplayer.c`.
- Keep the stock `bluetoothd` binary available. The patched daemon sits at `/usr/libexec/bluetooth/browse/bluetoothd`; both binaries keep the name `bluetoothd` so process lookup still works.
- Never ship a prebuilt daemon in this branch. A firmware build stages the stripped daemon in the packer assets.

## Architecture

`btbrowser.c` serves `org.bluez.MediaFolder1` and `org.bluez.MediaItem1` from the library handles that `btplayer.c` owns. The root path is `/org/mpris/MediaPlayer2/Filesystem`. It returns `Artists`, `Albums` and `Tracks`. Artist folders return album folders for that artist. Album folders return track items. Track items carry `Name`, `Type`, `Player` and a `Metadata` dict with `Title` and `Artist`.

`btbrowser.c` strips the library's internal `0x1F` album key from labels and keeps the raw key for SQL filters. It parses `Start` and `End` as uint32, treats `End` as inclusive, clamps a page to 24 items and 14 KB, retries once on a stale handle, and accepts `ChangeFolder` for the root and every registered folder. Its folder table holds 4096 entries. Nested item paths such as `.../item/00000001` reach the Play handler, which posts the real file path to the GUI thread.

`dbuslite` exposes `dbus_w_pad()` to align variable-length `a{oa{sv}}` tuples.

`tools/bluez-browse/` holds the BlueZ 5.54 patch, four helper modules, a build recipe and host harnesses. `avrcp-tg-browse.c` encodes the TG PDUs and clamps them to 505 bytes. `media-browse.c` calls the app's folder and item interfaces with a 3000 ms deadline, one retry and exact signature checks. It keeps a 512-entry LRU cache that maps `xesam:url` values to stable 64-bit UIDs. `tg-pending.c` matches async replies to the current transaction. `avctp-defer.c` holds a frame until the D-Bus calls finish.

`bluetoothd_fallback.c` selects the daemon. It shares one 8000 ms deadline between the service and adapter waits. It kills a failed patched daemon before starting stock. It refuses to start stock when that kill fails, so two daemons never race for `org.bluez`.

## Tradeoffs

**Stock fallback.** A patched daemon is the only way to answer the browsing PDUs. Replacing a system daemon can change pairing or codec behavior. Stock stays available and seven fallback cases cover the transitions.

**File flag.** Browsing starts on. A file on the writable partition restores stock behavior. A UI setting would have added GUI work and left no recovery path when the UI could not load.

**Image size.** The packer limits `rootfs.squashfs` to 45 MiB (`ROOTFS_MAX = 0x2d00000`). The extra daemon occupies most of the remaining headroom. This branch has no packer gate of its own.

**No CI device run.** The harnesses run locally. The repository workflows check bug logs, issue titles and languages. None runs the harnesses or a car. One head unit test exists outside CI, and one passing head unit says nothing about another.

## Test gates

| Gate | Cases | Scope |
|---|---:|---|
| PDU conformance | 189 | byte encodings, UIDs, status codes, folder counts |
| D-Bus | 50 | signatures, reply types, true folder count, error paths |
| AVCTP | 14 | frame save, header bytes, cleanup |
| Pending requests | 12 | current, stale, per-player cleanup |
| Fallback | 7 cases, 35 checks | patched success, disable flag, spawn failure, kill failure, kill refusal, shared deadline, both fail |
| App harness | 74 checks | folder descent, back navigation, strict filters, stale retry, nested Play, `GetAll` |

`RECIPE.sh host` applies the patches, builds BlueZ and checks the vendor strings. `RECIPE.sh cross` needs a staged R1 sysroot with `glib-2.0.pc` and `dbus-1.pc`; it exits 3 when those files are missing.

## Review journey

1. The first round found the app never dispatched Play for nested artist and album items, and `ChangeFolder` rejected the browse root. We stripped the index before `/item`, accepted the root, capped root listings at three children, and deduplicated folder entries.
2. The second round found a wrong breadcrumb in `SetBrowsedPlayer` and a UID cache that cleared on folder changes. We sent depth zero, added a reset callback for re-selection, kept the cache across navigation, and logged D-Bus error names.
3. The third round found a use-after-free for superseded replies, a bus-wide cancel, and a 128-folder cap. We scoped pending cleanup to the player, scoped cancel to the destination, and raised the folder cap to 4096.
4. The fourth round found the count PDU reported one page instead of the folder total, folders advertised Play without a Play method, and no test for a repeated `SetBrowsedPlayer`. We read `NumberOfItems`, set `Playable` false on folders, and added guard tests.
5. The fifth round checked the two wires. D-Bus alignment and AVCTP framing could close the connection. We tightened the writers and added byte-level cases.
6. The sixth round found the deferred reply wrote twice, the total never reached AVRCP, the deferred PDU skipped the clamp, and `ChangePath` read its UID four bytes early. We suppressed the immediate write, read the total synchronously, clamped the deferred PDU and corrected the offset.
7. The seventh round found the daemon called MPRIS `TrackList` while the app exposes `MediaFolder1`, counted folders on the player path instead of the folder path, played a synthetic URI, and never changed the browsed folder. We rewrote the bridge to the app interfaces, passed folder paths, played item object paths, and wired `ChangeFolder`.
