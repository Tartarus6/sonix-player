#!/bin/bash
# Rebuild the BlueZ 5.54 TG-browsing bluetoothd.
#
#   RECIPE.sh [host|cross]
#
# The default is `cross'.  With `host' the patched tree is built with the
# host compiler; this is useful for checking that the patches still compile
# when the MIPS toolchain is not available yet.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$ROOT/../.." && pwd)
MODE=${1:-cross}

TMPDIR=${TMPDIR:-/home/diogo/Downloads/Hiby/pr-work/tmp}
BUILD=$TMPDIR/bluez-browse-build
SYSROOT=$BUILD/r1-rootfs
TARBALL=$TMPDIR/bluez-5.54.tar.xz
SRC=$BUILD/bluez-5.54
OUT=$BUILD/out

TARBALL_URL=https://www.kernel.org/pub/linux/bluetooth/bluez-5.54.tar.xz
TARBALL_SHA=68cdab9e63e8832b130d5979dc8c96fdb087b31278f342874d992af3e56656dc
UPT_URL=https://github.com/CookieCaptainD/sonix-player/releases/download/bt-car-mode-pin-test/r1_car_mode.upt
UPT=$TMPDIR/r1_car_mode.upt

TOOLCHAIN=$REPO/sonix-player/rockbox-toolchain/bin
CROSS=mipsel-rockbox-linux-gnu

mkdir -p "$TMPDIR" "$BUILD" "$OUT"

say() { printf '\n==> %s\n' "$*"; }

# -----------------------------------------------------------------------
say 'Fetch and pin the BlueZ 5.54 source'
# -----------------------------------------------------------------------
if [ ! -f "$TARBALL" ] || ! sha256sum -c --status <<<"$TARBALL_SHA  $TARBALL"; then
	curl -fL --retry 3 -o "$TARBALL" "$TARBALL_URL"
fi
echo "$TARBALL_SHA  $TARBALL" | sha256sum -c -

rm -rf "$SRC"
tar xf "$TARBALL" -C "$BUILD"

# -----------------------------------------------------------------------
say 'Apply patches in filename order'
# -----------------------------------------------------------------------
for p in "$ROOT"/patches/*.patch; do
	echo "patch: $(basename "$p")"
	patch -d "$SRC" -p1 --batch --forward <"$p"
done

# -----------------------------------------------------------------------
say 'Copy browsing modules into profiles/audio'
# -----------------------------------------------------------------------
for f in "$ROOT"/modules/*.[ch]; do
	cp -p "$f" "$SRC/profiles/audio/"
done

cd "$SRC"

# Makefile.in is generated; Makefile.am and Makefile.plugins carry the new
# sources, so regenerate it before configuring.
say 'Regenerate the build system'
autoreconf -fi

# -----------------------------------------------------------------------
say 'Prepare the stock R1 sysroot'
# -----------------------------------------------------------------------
if [ ! -d "$SYSROOT/usr/lib" ]; then
	rm -rf "$BUILD/r1-upt" "$BUILD/r1-rootfs.squashfs"
	mkdir -p "$BUILD/r1-upt"
	if [ ! -s "$UPT" ]; then
		curl -fL --retry 3 -o "$UPT" "$UPT_URL"
	fi
	7z x -y -o"$BUILD/r1-upt" "$UPT" >/dev/null
	cat "$BUILD"/r1-upt/ota_v0/rootfs.squashfs.* >"$BUILD/r1-rootfs.squashfs"
	unsquashfs -d "$SYSROOT" "$BUILD/r1-rootfs.squashfs" >/dev/null
fi

# -----------------------------------------------------------------------
say "Configure ($MODE)"
# -----------------------------------------------------------------------
COMMON_CONFIGURE_ARGS=(
	--disable-dependency-tracking
	--enable-library=no
	--disable-tools
	--disable-monitor
	--disable-cups
	--disable-obex
	--disable-client
	--disable-systemd
	--disable-udev
	--enable-avrcp
)

if [ "$MODE" = host ]; then
	./configure "${COMMON_CONFIGURE_ARGS[@]}"
else
	if [ ! -x "$TOOLCHAIN/$CROSS-gcc" ]; then
		echo "BLOCKER: cross compiler $TOOLCHAIN/$CROSS-gcc is not available" >&2
		exit 2
	fi
	if ! PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/lib/pkgconfig \
			pkg-config --exists glib-2.0 dbus-1; then
		echo "BLOCKER: R1 sysroot has no glib-2.0/dbus-1 .pc files; cross libraries" >&2
		echo "        must be staged into $SYSROOT before a cross build" >&2
		exit 3
	fi
	PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/lib/pkgconfig \
	./configure "${COMMON_CONFIGURE_ARGS[@]}" \
		--host=$CROSS \
		CC="$TOOLCHAIN/$CROSS-gcc" \
		AR="$TOOLCHAIN/$CROSS-ar" \
		RANLIB="$TOOLCHAIN/$CROSS-ranlib" \
		CFLAGS="--sysroot=$SYSROOT -O2" \
		LDFLAGS="--sysroot=$SYSROOT"
fi

say 'Build bluetoothd'
make -j"$(nproc)"

BIN=$SRC/src/bluetoothd
file "$BIN"

# -----------------------------------------------------------------------
say 'ABI check'
# -----------------------------------------------------------------------
if [ "$MODE" = cross ]; then
	python3 "$ROOT/abicheck.py" "$SYSROOT" "$SYSROOT/usr/lib" "$BIN" || \
		echo "WARNING: abicheck reported a problem (see above)" >&2
else
	echo 'skipped (host build against the host C library)'
fi

# -----------------------------------------------------------------------
say 'Strip and stage'
# -----------------------------------------------------------------------
if [ "$MODE" = host ]; then
	strip --strip-unneeded "$BIN" -o "$OUT/bluetoothd"
else
	"$TOOLCHAIN/$CROSS-strip" --strip-unneeded "$BIN" -o "$OUT/bluetoothd"
fi
file "$OUT/bluetoothd"
ls -l "$OUT/bluetoothd"

# -----------------------------------------------------------------------
say 'Vendor compatibility checks'
# -----------------------------------------------------------------------
if [ "$MODE" = cross ]; then
	"$ROOT/verify-vendor.sh" "$OUT/bluetoothd" \
		"$SYSROOT/usr/libexec/bluetooth/bluetoothd"
else
	"$ROOT/verify-vendor.sh" "$OUT/bluetoothd"
fi

say "Done: $OUT/bluetoothd"
