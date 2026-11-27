# AVRCP target browsing plan

Date: 2026-11-27
Base: Tartarus6/sonix-player v1.3.3, commit `c54f8e4`
Design: `docs/superpowers/specs/2026-11-27-avrcp-target-browsing-design.md`

## Work order

1. Record the v1.3.3 commit. Keep `btplayer.c` on metadata and transport. Put browsing in `btbrowser.c`.
2. Add `dbus_w_pad()` to `dbuslite` and build the folder tree: root with `Artists`, `Albums`, `Tracks`, plus nested artist and album folders. Strip album keys for labels and keep them for filters.
3. Implement strict filters, inclusive `End`, page and byte clamps, stale retry, root `ChangeFolder`, and nested item Play that posts a real file path to the GUI thread.
4. Rewrite the daemon bridge to call `org.bluez.MediaFolder1` and `org.bluez.MediaItem1` with a 3000 ms deadline and one retry. Keep a 512-entry URI to UID cache. Read totals from `NumberOfItems`.
5. Add the BlueZ 5.54 patch: TG handlers, PDU clamps, deferred replies, pending cleanup, browsable feature bit, HiBy vendor preservation.
6. Add `bluetoothd_fallback.c`. Start the patched daemon unless `/usr/data/disable_bt_browse` exists. Use one 8000 ms deadline. Kill a failed daemon before starting stock.
7. Add the D-Bus policy rules for R1 and R3PII and repair the XML comments.
8. Run every harness and the host recipe. Add Car Mode and PIN screenshots under `docs/screenshots/`. Browsing has no player UI. Write the PR body with the v1.3.3 comparison.

## Gates

| Gate | Cases | Rule |
|---|---:|---|
| PDU conformance | 189 | all pass |
| D-Bus | 50 | all pass |
| AVCTP | 14 | all pass |
| Pending requests | 12 | all pass |
| Fallback | 7 cases, 35 checks | all pass |
| App harness | 74 checks | all pass |

Run `tools/bluez-browse/RECIPE.sh host` and every `run*.sh` under `tools/bluez-browse/tests/`. Run `sonix-player/tests/run_btbrowser_test.sh` and `sonix-player/tests/harness/run.sh`. `RECIPE.sh cross` needs a staged sysroot with `glib-2.0.pc` and `dbus-1.pc`.

## Risks in order

1. A patched daemon can change pairing or codec behavior. Stock fallback and seven transition cases contain the risk.
2. A car may ignore the folder tree even when the PDUs are correct. One head unit test exists outside CI.
3. The packer limits `rootfs.squashfs` to 45 MiB. The extra daemon uses most of the headroom.
4. Cross build needs a staged sysroot. Local host build and harnesses cover the patch. CI has no harness job.
