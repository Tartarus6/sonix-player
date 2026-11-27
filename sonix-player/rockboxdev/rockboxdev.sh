#!/bin/bash
set -e

script_dir=$(cd "$(dirname "$0")" && pwd)
prefix=${RBDEV_HOST_PREFIX:-$PWD/rockbox-toolchain}
downloads=${RBDEV_DOWNLOAD:-$PWD/tmp/rbdev-dl}
work=${RBDEV_BUILD:-$PWD/tmp/rbdev-build}
restart=${RBDEV_RESTART:-}
: "${GNU_MIRROR:=https://mirrors.kernel.org/gnu}"
: "${LINUX_MIRROR:=https://cdn.kernel.org/pub/linux}"

die() { echo "ROCKBOXDEV: $*" >&2; exit 1; }

for arg in "$@"; do
    case "$arg" in
        --prefix=*) prefix=${arg#*=} ;;
        --dlwhere=*) downloads=${arg#*=} ;;
        --builddir=*) work=${arg#*=} ;;
        --makeflags=*) export MAKEFLAGS=${arg#*=} ;;
        --restart=*) restart=${arg#*=} ;;
        --help) echo "Usage: $0 [--prefix=PATH] [--dlwhere=PATH] [--builddir=PATH] [--makeflags=FLAGS] [--restart=STEP]"; exit 0 ;;
        *) die "Unknown option: $arg" ;;
    esac
done

make=$(command -v gmake || command -v make) || die "GNU Make is required."
if [ "$(uname -s)" = Darwin ]; then
    jobs=$(sysctl -n hw.physicalcpu)
else
    jobs=$(nproc)
fi
for tool in gcc g++ wget tar xz bzip2 gzip patch makeinfo m4 bison flex gawk; do
    command -v "$tool" >/dev/null || die "Install $tool before building the toolchain. See README.md."
done
[[ $($make --version) == *'GNU Make'* ]] || die "GNU Make is required."

# ALSA's bundled config.guess predates ARM64. Use a current one to pass
# --build to configure instead of modifying the generated source files.
config_guess=${RBDEV_CONFIG_GUESS:-$(command -v config.guess || true)}
for candidate in /usr/share/misc/config.guess /usr/share/automake-*/config.guess \
                 "${HOMEBREW_PREFIX:-/opt/homebrew}"/share/automake-*/config.guess; do
    [ -z "$config_guess" ] || break
    if [ -x "$candidate" ]; then config_guess=$candidate; fi
done
[ -n "$config_guess" ] && build_host=$("$config_guess") ||
    die "Install autotools-dev or set RBDEV_CONFIG_GUESS to a current config.guess."
export M4=${M4:-$(command -v m4)}

mkdir -p "$prefix" "$downloads" "$work"
prefix=$(cd "$prefix" && pwd)
downloads=$(cd "$downloads" && pwd)
work=$(cd "$work" && pwd)

if ! printf '#include <gmp.h>\n#include <mpfr.h>\n#include <mpc.h>\nint main(void) { return 0; }\n' |
    gcc -x c - -lmpc -lmpfr -lgmp -o "$work/check-host-libraries"; then
    die "Install the GMP, MPFR and MPC development packages. See README.md."
fi
rm -f "$work/check-host-libraries"

stamps=$prefix/.stages
mkdir -p "$stamps"
steps=(binutils gcc-stage1 linux-headers glibc gcc-stage2 alsa-lib)
if [ -n "$restart" ]; then
    start=-1
    for i in "${!steps[@]}"; do
        if [ "${steps[i]}" = "$restart" ]; then start=$i; break; fi
    done
    [ "$start" -ge 0 ] || die "Unknown restart step: $restart"
    for ((i=0; i<start; i++)); do
        [ -f "$stamps/${steps[i]}" ] || die "${steps[i]} hasn't completed; retry without --restart."
    done
    rm -f "$prefix/.sonix-ready"
    for step in "${steps[@]:start}"; do rm -f "$stamps/$step"; done
fi

fetch() {
    local url=$1 archive=$downloads/${1##*/}
    if [ ! -s "$archive" ]; then
        echo "ROCKBOXDEV: Downloading $url"
        if command -v curl >/dev/null; then
            curl -fL --connect-timeout 30 -o "$archive.part" "$url" || die "Download failed: $url"
        else
            wget -T 60 -O "$archive.part" "$url" || die "Download failed: $url"
        fi
        mv -f "$archive.part" "$archive"
    fi
}

run() {
    printf 'Running %s\n' "$*" >> "$log"
    "$@" >> "$log" 2>&1 || die "See $log"
}

# Each stage starts with untouched sources. Keep downloads and installed
# stages after failure; leave the failed stage's log and sources for diagnosis.
build_stage() {
    local step=$1 source=$2 url=$3 patch_file=$4
    shift 4
    if [ -f "$stamps/$step" ]; then
        echo "ROCKBOXDEV: Skipping completed step '$step'"
        return
    fi
    local log=$work/build-$step.log
    echo "ROCKBOXDEV: Building '$step' (log: $log)"
    : > "$log"
    fetch "$url"
    cd "$work"
    rm -rf "$source" "build-$step"
    run tar xf "$downloads/${url##*/}"
    if [ -n "$patch_file" ]; then
        (cd "$source" && run patch -p1 --batch --forward < "$script_dir/toolchain-patches/$patch_file")
    fi
    mkdir "build-$step"
    cd "build-$step"
    if [ "$step" = linux-headers ]; then
        cp -pR "../$source/." .
        run "$make" -j"$jobs" O=. ARCH=mips "INSTALL_HDR_PATH=$sysroot/usr/" headers_install
        run "$make" O=. ARCH=mips "INSTALL_HDR_PATH=$sysroot/usr/" headers_check
    else
        local cfg="../$source/configure" install_prefix=$prefix
        local cflags='-U_FORTIFY_SOURCE -fgnu89-inline -O2' cxxflags=''
        local install_args=()
        case "$step" in
            gcc-stage1) cxxflags=-std=gnu++03 ;;
            gcc-stage2) cxxflags=-std=gnu++11 ;;
            glibc) cflags+=' -fcommon'; install_prefix=/usr; install_args=("install_root=$sysroot") ;;
            alsa-lib)
                cp -pR "../$source/." .
                cfg=./configure
                install_prefix=/usr
                install_args=("DESTDIR=$sysroot")
                ;;
        esac
        CFLAGS="$cflags" CXXFLAGS="$cxxflags" run "$cfg" "--prefix=$install_prefix" --disable-docs --disable-tests "$@"
        run "$make" -j"$jobs"
        run "$make" install "${install_args[@]}"
    fi
    cd "$work"
    touch "$stamps/$step"
    rm -rf "$source" "build-$step"
}

target=mipsel-rockbox-linux-gnu
sysroot=$prefix/$target/sysroot
export PATH="$prefix/bin:$PATH"
# Run outside the caller's Make jobserver. The bootstrap has its own parallelism.
if [[ ${MAKEFLAGS:-} == *--jobserver* ]]; then unset MAKEFLAGS; fi

build_stage binutils binutils-2.38 "$GNU_MIRROR/binutils/binutils-2.38.tar.xz" binutils-c23.patch \
    "--target=$target" --disable-werror "--with-sysroot=$sysroot" --disable-nls
build_stage gcc-stage1 gcc-9.5.0 "$GNU_MIRROR/gcc/gcc-9.5.0/gcc-9.5.0.tar.xz" '' \
    --enable-languages=c "--target=$target" --without-headers --disable-threads \
    --disable-libgomp --disable-libmudflap --disable-libssp --disable-libquadmath \
    --disable-shared --with-newlib --disable-libitm --disable-libsanitizer --disable-libatomic
build_stage linux-headers linux-3.10.108 "$LINUX_MIRROR/kernel/v3.x/linux-3.10.108.tar.gz" linux-c23.patch
build_stage glibc glibc-2.27 "$GNU_MIRROR/glibc/glibc-2.27.tar.xz" glibc-227-make44.patch \
    "--target=$target" "--host=$target" "--build=$build_host" --with-__thread \
    "--with-headers=$sysroot/usr/include" --enable-kernel=3.2 --enable-oldest-abi=2.16 --disable-werror
build_stage gcc-stage2 gcc-9.5.0 "$GNU_MIRROR/gcc/gcc-9.5.0/gcc-9.5.0.tar.xz" '' \
    --enable-languages=c,c++ "--target=$target" "--with-sysroot=$sysroot"
# Sonix links ALSA and implements its own D-Bus client.
build_stage alsa-lib alsa-lib-1.0.26 'https://www.alsa-project.org/files/pub/lib/alsa-lib-1.0.26.tar.bz2' '' \
    "--host=$target" "--build=$build_host" --disable-python

touch "$prefix/.sonix-ready"
echo "ROCKBOXDEV: Hiby OS toolchain build complete."
