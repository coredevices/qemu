#!/bin/bash
# Build script for Pebble QEMU fork.
# Builds qemu-system-arm and bundles a relocatable distribution under ./dist.
#
# Usage: bash build-dist.sh
#
# Prerequisites:
#   macOS:         brew install sdl2 pixman glib pkg-config ninja
#   Debian/Ubuntu: sudo apt install libsdl2-dev libpixman-1-dev libglib2.0-dev \
#                                   pkg-config ninja-build python3-venv build-essential
set -euo pipefail

OS="$(uname -s)"
echo "=== Host: ${OS} $(uname -m) ==="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
VENV_DIR="${SCRIPT_DIR}/.venv"

# Use system meson/ninja if available, otherwise create a venv
if command -v meson &>/dev/null && command -v ninja &>/dev/null; then
    PYTHON="python3"
else
    if [ ! -d "${VENV_DIR}" ]; then
        python3 -m venv "${VENV_DIR}"
        "${VENV_DIR}/bin/pip" install meson ninja distlib tomli
    fi
    export PATH="${VENV_DIR}/bin:$PATH"
    PYTHON="${VENV_DIR}/bin/python3"
fi

mkdir -p "${BUILD_DIR}"
rm -f "${BUILD_DIR}/build.ninja"

cd "${BUILD_DIR}"

"${SCRIPT_DIR}/configure" \
    --target-list=arm-softmmu \
    --python="${PYTHON}" \
    --enable-sdl \
    --disable-tools \
    --disable-docs \
    --disable-guest-agent \
    --disable-werror \
    \
    `# UI / display backends — keep SDL, Cocoa, VNC` \
    --disable-curses \
    --disable-gtk \
    --disable-vte \
    --disable-vnc-jpeg \
    --disable-spice \
    --disable-spice-protocol \
    --disable-dbus-display \
    --disable-sdl-image \
    --disable-opengl \
    --disable-virglrenderer \
    --disable-rutabaga-gfx \
    --disable-pvg \
    \
    `# Crypto / TLS / auth — Pebble has no networking` \
    --disable-gnutls \
    --disable-gcrypt \
    --disable-nettle \
    --disable-libssh \
    --disable-auth-pam \
    --disable-libcbor \
    --disable-crypto-afalg \
    \
    `# USB / smartcard / security devices` \
    --disable-libusb \
    --disable-usb-redir \
    --disable-libudev \
    --disable-smartcard \
    --disable-u2f \
    --disable-canokey \
    --disable-tpm \
    --disable-brlapi \
    \
    `# Network backends — Pebble has no NIC` \
    --disable-slirp \
    --disable-vde \
    --disable-vmnet \
    --disable-netmap \
    --disable-l2tpv3 \
    --disable-af-xdp \
    --disable-bpf \
    \
    `# Block / image formats — Pebble has no disk` \
    --disable-bzip2 \
    --disable-lzfse \
    --disable-lzo \
    --disable-snappy \
    --disable-zstd \
    --disable-qatzip \
    --disable-qpl \
    --disable-bochs \
    --disable-cloop \
    --disable-dmg \
    --disable-parallels \
    --disable-qcow1 \
    --disable-qed \
    --disable-vdi \
    --disable-vhdx \
    --disable-vmdk \
    --disable-vpc \
    --disable-vvfat \
    --disable-blkio \
    --disable-curl \
    --disable-glusterfs \
    --disable-libiscsi \
    --disable-libnfs \
    --disable-rbd \
    --disable-libpmem \
    --disable-libdaxctl \
    --disable-replication \
    --disable-fuse \
    --disable-fuse-lseek \
    --disable-virtfs \
    --disable-attr \
    --disable-mpath \
    --disable-linux-aio \
    --disable-linux-io-uring \
    \
    `# Virtio / vhost — not used by Pebble machines` \
    --disable-vhost-crypto \
    --disable-vhost-kernel \
    --disable-vhost-net \
    --disable-vhost-user \
    --disable-vhost-user-blk-server \
    --disable-vhost-vdpa \
    --disable-libvduse \
    --disable-vduse-blk-export \
    --disable-hv-balloon \
    \
    `# Accelerators — TCG only for Cortex-M` \
    --disable-kvm \
    --disable-hvf \
    --disable-whpx \
    --disable-xen \
    \
    `# Misc unused subsystems` \
    --disable-capstone \
    --disable-gio \
    --disable-numa \
    --disable-rdma \
    --disable-seccomp \
    --disable-selinux \
    --disable-libdw \
    --disable-libkeyutils \
    --disable-multiprocess \
    --disable-vfio-user-server \
    --disable-modules \
    --disable-plugins \
    --disable-rust \
    --disable-sndio 2>&1

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
ninja -j"${NPROC}" qemu-system-arm 2>&1

echo ""
echo "=== Build complete ==="
echo "Binary: ${BUILD_DIR}/qemu-system-arm"

# === Bundle distributable ===
DIST_DIR="${SCRIPT_DIR}/dist"
echo ""
echo "=== Bundling distributable ==="
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/bin"

cp "${BUILD_DIR}/qemu-system-arm" "${DIST_DIR}/bin/qemu-pebble"
strip "${DIST_DIR}/bin/qemu-pebble"

# Bundle data files (watch decorations) at the relocated CONFIG_QEMU_DATADIR
# layout: dist/share/qemu/pebble-decorations/. qemu-pebble locates them at
# runtime via get_relocated_path(CONFIG_QEMU_DATADIR).
mkdir -p "${DIST_DIR}/share/qemu/pebble-decorations"
cp "${SCRIPT_DIR}/pc-bios/pebble-decorations/"*.png \
    "${DIST_DIR}/share/qemu/pebble-decorations/"

# Keymaps — SDL display backend loads these at runtime relative to the
# binary at lib/pc-bios/keymaps.
mkdir -p "${DIST_DIR}/lib/pc-bios/keymaps"
cp "${SCRIPT_DIR}/pc-bios/keymaps/"* "${DIST_DIR}/lib/pc-bios/keymaps/"

if [ "$OS" = "Darwin" ] && command -v brew &>/dev/null; then
    mkdir -p "${DIST_DIR}/lib"
    for lib in \
        "$(brew --prefix pixman)/lib/libpixman-1.0.dylib" \
        "$(brew --prefix sdl2)/lib/libSDL2-2.0.0.dylib" \
        "$(brew --prefix glib)/lib/libglib-2.0.0.dylib" \
        "$(brew --prefix glib)/lib/libgmodule-2.0.0.dylib" \
        "$(brew --prefix gettext)/lib/libintl.8.dylib" \
        "$(brew --prefix pcre2)/lib/libpcre2-8.0.dylib"; do
        if [ -f "$lib" ]; then
            cp "$lib" "${DIST_DIR}/lib/"
            echo "  -> lib/$(basename "$lib")"
        else
            echo "  WARNING: $lib not found"
        fi
    done

    chmod u+w "${DIST_DIR}/lib/"*.dylib
    chmod u+w "${DIST_DIR}/bin/qemu-pebble"

    echo "  Fixing up dylib paths with otool/install_name_tool..."
    BINARY="${DIST_DIR}/bin/qemu-pebble"

    for lib in "${DIST_DIR}/lib/"*.dylib; do
        libname=$(basename "$lib")
        old_path=$(otool -L "$BINARY" | awk -v n="$libname" 'index($1, n) {print $1; exit}')
        if [ -n "$old_path" ]; then
            install_name_tool -change "$old_path" "@executable_path/../lib/$libname" "$BINARY"
        fi
        install_name_tool -id "@loader_path/$libname" "$lib"
        for other_lib in "${DIST_DIR}/lib/"*.dylib; do
            other_name=$(basename "$other_lib")
            [ "$libname" = "$other_name" ] && continue
            old_ref=$(otool -L "$lib" | awk -v n="$other_name" 'index($1, n) {print $1; exit}')
            if [ -n "$old_ref" ]; then
                install_name_tool -change "$old_ref" "@loader_path/$other_name" "$lib"
            fi
        done
    done

    codesign --force --sign - "$BINARY"
    for lib in "${DIST_DIR}/lib/"*.dylib; do
        codesign --force --sign - "$lib"
    done
    echo "  Re-signed binary and libs"
else
    # Linux: no libs bundled — users install runtime deps via their package
    # manager (see Prerequisites at the top of this script).
    echo "  no libs bundled — runtime deps must be installed by the user"
fi

echo ""
echo "=== Distributable ready ==="
echo "  ${DIST_DIR}/bin/qemu-pebble"
if [ -d "${DIST_DIR}/lib" ]; then
    echo "  ${DIST_DIR}/lib/"
fi
