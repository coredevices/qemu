#!/bin/bash
# Build static third-party libraries for the macOS qemu-pebble dist.
#
# Downloads pinned upstream sources and installs static-only libraries
# into a staging prefix. build-dist.sh links qemu against these so the
# shipped binary depends only on macOS system libraries — no Homebrew,
# no bundled dylibs, no sdl2-compat/SDL3 indirection.
#
# Usage: bash build-static-deps.sh [--prefix DIR]
#
# Prerequisites: Apple CLT (clang/make), pkg-config, meson, ninja, curl.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${SCRIPT_DIR}/build/static-deps"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Pinned versions + tarball sha256s. Bump here (and refresh the sha) to
# update a dependency; the .stamp mechanism rebuilds automatically.
LIBFFI_VERSION=3.5.2
LIBFFI_SHA256=f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc
GETTEXT_VERSION=0.26
GETTEXT_SHA256=39acf4b0371e9b110b60005562aace5b3631fed9b1bb9ecccfc7f56e58bb1d7f
PCRE2_VERSION=10.46
PCRE2_SHA256=15fbc5aba6beee0b17aecb04602ae39432393aba1ebd8e39b7cabf7db883299f
LIBPNG_VERSION=1.6.50
LIBPNG_SHA256=708f4398f996325819936d447f982e0db90b6b8212b7507e7672ea232210949a
PIXMAN_VERSION=0.46.4
PIXMAN_SHA256=d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c
SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
GLIB_VERSION=2.86.1
GLIB_SHA256=119d1708ca022556d6d2989ee90ad1b82bd9c0d1667e066944a6d0020e2d5e57

# Rebuild only when this script (versions, flags) changes.
STAMP="${PREFIX}/.stamp"
SELF_HASH="$(shasum -a 256 "${BASH_SOURCE[0]}" | awk '{print $1}')"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${SELF_HASH}" ]; then
    echo "=== Static deps up to date: ${PREFIX} ==="
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
rm -rf "${PREFIX}"
mkdir -p "${PREFIX}"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# All builds must see only the staging prefix, never Homebrew or an
# inherited environment (PKG_CONFIG_PATH is searched before LIBDIR).
export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"
unset PKG_CONFIG_PATH
export CPPFLAGS="-I${PREFIX}/include"
export LDFLAGS="-L${PREFIX}/lib"

fetch() { # <url> <sha256> -> extracted top-level dir (cd'd into)
    local url="$1" sha="$2" file dir
    file="${WORK}/$(basename "${url}")"
    echo "==> Downloading $(basename "${url}")"
    curl -fsSL --retry 3 -o "${file}" "${url}"
    echo "${sha}  ${file}" | shasum -a 256 -c - >/dev/null
    # || true: head's early exit SIGPIPEs tar, which pipefail would
    # otherwise turn into a failure; a truly bad archive fails the cd.
    dir="$(tar -tf "${file}" | head -1 | cut -d/ -f1 || true)"
    tar -xf "${file}" -C "${WORK}"
    cd "${WORK}/${dir}"
}

build_autotools() { # <configure args...>
    ./configure --prefix="${PREFIX}" --disable-shared --enable-static "$@" \
        >/dev/null
    make -j"${JOBS}" >/dev/null
    make install >/dev/null
}

build_meson() { # <meson setup args...>
    meson setup _build --prefix="${PREFIX}" --libdir=lib \
        -Ddefault_library=static -Dbuildtype=release "$@" >/dev/null
    ninja -C _build install >/dev/null
}

# zlib comes from the macOS SDK, not the staging prefix — but staged
# .pc files (libpng) declare "Requires: zlib", which the hermetic
# PKG_CONFIG_LIBDIR could not otherwise satisfy.
mkdir -p "${PREFIX}/lib/pkgconfig"
cat > "${PREFIX}/lib/pkgconfig/zlib.pc" <<'EOF'
Name: zlib
Description: macOS system zlib
Version: 1.2.12
Libs: -lz
EOF

echo "=== Building static deps into ${PREFIX} ==="

echo "=== libffi ${LIBFFI_VERSION} (glib build dependency) ==="
fetch "https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz" "${LIBFFI_SHA256}"
build_autotools --disable-docs

echo "=== gettext ${GETTEXT_VERSION} (libintl only) ==="
fetch "https://ftp.gnu.org/gnu/gettext/gettext-${GETTEXT_VERSION}.tar.gz" "${GETTEXT_SHA256}"
cd gettext-runtime
build_autotools --disable-java --disable-libasprintf

echo "=== pcre2 ${PCRE2_VERSION} ==="
fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VERSION}/pcre2-${PCRE2_VERSION}.tar.bz2" "${PCRE2_SHA256}"
build_autotools

echo "=== libpng ${LIBPNG_VERSION} ==="
fetch "https://download.sourceforge.net/libpng/libpng-${LIBPNG_VERSION}.tar.gz" "${LIBPNG_SHA256}"
build_autotools

echo "=== pixman ${PIXMAN_VERSION} ==="
fetch "https://www.cairographics.org/releases/pixman-${PIXMAN_VERSION}.tar.gz" "${PIXMAN_SHA256}"
build_meson -Dtests=disabled -Ddemos=disabled

echo "=== SDL2 ${SDL2_VERSION} ==="
fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz" "${SDL2_SHA256}"
build_autotools

echo "=== glib ${GLIB_VERSION} ==="
fetch "https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-${GLIB_VERSION}.tar.xz" "${GLIB_SHA256}"
# sysprof=disabled: keeps meson from git-fetching the sysprof
# subproject — everything built here must come from the pinned
# tarballs above.
build_meson -Dtests=false -Dintrospection=disabled -Dsysprof=disabled

echo "${SELF_HASH}" > "${STAMP}"
echo "=== Static deps ready: ${PREFIX} ==="
