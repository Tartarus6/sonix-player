# BlueZ 5.54 TG browsing toolkit

Rebuilds the HiBy R1 `bluetoothd` from a pinned upstream BlueZ 5.54 tarball,
adding AVRCP 1.5 target browsing and the vendor notification socket.

## Layout

- `patches/` — numbered patches applied in filename order.
- `modules/` — source files copied into `profiles/audio/` by `RECIPE.sh`.
- `tests/` — host harnesses; each prints an exact check count.
- `RECIPE.sh` — download, patch, build, strip and verify.
- `verify-vendor.sh` — vendor-visible string / ELF ABI checks.
- `abicheck.py` — copy of the repo's ELF ABI checker.

## Patches

| Patch | Purpose |
| --- | --- |
| 01 | `storagedir` → `/usr/data/bluetooth` |
| 02 | `src/client.{c,h}` notify socket and source/sink/HID/metadata producers |
| 03 | force a persistent static address |
| 04 | raise daemon priority for the Broadcom controller |
| 05 | let the shell stop discovery started by another client |
| 09 | host build fix (`pause` → `media_pause`) |
| 11 | TG callbacks in `avrcp.h` |
| 12 | deferred browsing replies (`avctp_send_browsing_rsp`) |
| 13 | TG PDU handlers, stale-pending guard, cleanup and module sources |
| 14 | media retention, async D-Bus bridge, UID cache and true totals |
| 15 | advertise browsing in `features[7]` (`0x60` → `0x68`) |

## Tests

```
tests/run.sh                # 189 byte-level AVRCP checks
tests/run-dbus-sim.sh       #  50 media-browse checks (private D-Bus)
tests/run-defer.sh          #  14 AVCTP defer checks
tests/run-tg-pending.sh     #  12 pending-transaction checks
tests/run-parent.sh         #   6 path checks
tests/run-item-type.sh      #   3 item-type checks
tests/run-change-count.sh   #   3 count checks
tests/run-display-name.sh   #   2 display-name checks
tests/test_review_guards.sh # aggregate
```

Set `TMPDIR` before running; all builds go under `$TMPDIR`.

## Build

```
TMPDIR=/home/diogo/Downloads/Hiby/pr-work/tmp ./RECIPE.sh host    # host compile check
TMPDIR=/home/diogo/Downloads/Hiby/pr-work/tmp ./RECIPE.sh cross   # MIPS bluetoothd
```

The cross build needs the toolchain at
`sonix-player/rockbox-toolchain/bin` and an R1 rootfs (downloaded and
unsquashfsed automatically).
