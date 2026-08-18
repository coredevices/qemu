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

if [ "$OS" = "Darwin" ]; then
    mkdir -p "${DIST_DIR}/lib"
    BINARY="${DIST_DIR}/bin/qemu-pebble"
    chmod u+w "$BINARY"

    # Library homes that won't exist on end-user machines.
    NONSYSTEM_RE='^(/opt/homebrew|/usr/local|/opt/local)'

    # Print the non-system dylibs a Mach-O file references. For a dylib this
    # includes its own install name until -id rewrites it below; both loops
    # tolerate that (copy is skipped, -change on the id is a no-op).
    non_system_deps() {
        otool -L "$1" | tail -n +2 | awk '{print $1}' \
            | grep -E "${NONSYSTEM_RE}" || true
    }

    # Copy the full closure of non-system dylibs into dist/lib. Walking the
    # closure instead of keeping a hardcoded list: a stale list once missed
    # libpng and shipped a bundle that only ran where Homebrew was installed.
    echo "  Bundling non-system dylibs..."
    found_new=1
    while [ "${found_new}" -eq 1 ]; do
        found_new=0
        for f in "$BINARY" "${DIST_DIR}/lib/"*.dylib; do
            [ -f "$f" ] || continue
            for ref in $(non_system_deps "$f"); do
                name=$(basename "$ref")
                [ -f "${DIST_DIR}/lib/${name}" ] && continue
                if [ ! -f "$ref" ]; then
                    echo "ERROR: $f references missing library $ref" >&2
                    exit 1
                fi
                cp "$ref" "${DIST_DIR}/lib/${name}"
                chmod u+w "${DIST_DIR}/lib/${name}"
                echo "  -> lib/${name}"
                found_new=1
            done
        done

        # Homebrew's sdl2 formula now installs sdl2-compat: a shim that
        # dlopens libSDL3 at runtime, invisible to otool -L. Without SDL3
        # the shim's constructor blocks in a modal error dialog before
        # main() runs. Its first dlopen candidate is
        # @loader_path/libSDL3.dylib, so bundle SDL3 next to it; the next
        # loop pass then walks SDL3's own deps and the fixup below
        # rewrites its paths.
        if [ -f "${DIST_DIR}/lib/libSDL2-2.0.0.dylib" ] \
            && [ ! -f "${DIST_DIR}/lib/libSDL3.dylib" ] \
            && strings "${DIST_DIR}/lib/libSDL2-2.0.0.dylib" \
                | grep -qxF '@loader_path/libSDL3.dylib'; then
            sdl3=""
            if command -v brew &>/dev/null; then
                sdl3="$(brew --prefix sdl3 2>/dev/null || true)/lib/libSDL3.dylib"
            fi
            if [ ! -f "$sdl3" ]; then
                echo "ERROR: bundled libSDL2 is the sdl2-compat shim but libSDL3.dylib was not found" >&2
                exit 1
            fi
            cp "$sdl3" "${DIST_DIR}/lib/libSDL3.dylib"
            chmod u+w "${DIST_DIR}/lib/libSDL3.dylib"
            echo "  -> lib/libSDL3.dylib (sdl2-compat runtime dependency)"
            found_new=1
        fi
    done

    echo "  Fixing up dylib paths with install_name_tool..."
    for lib in "${DIST_DIR}/lib/"*.dylib; do
        [ -f "$lib" ] || continue
        install_name_tool -id "@loader_path/$(basename "$lib")" "$lib"
    done
    for f in "$BINARY" "${DIST_DIR}/lib/"*.dylib; do
        [ -f "$f" ] || continue
        if [ "$f" = "$BINARY" ]; then
            prefix="@executable_path/../lib"
        else
            prefix="@loader_path"
        fi
        for ref in $(non_system_deps "$f"); do
            install_name_tool -change "$ref" "${prefix}/$(basename "$ref")" "$f"
        done
    done

    # A leftover non-system reference means the bundle only runs on machines
    # with the packager's library layout — fail the build instead.
    for f in "$BINARY" "${DIST_DIR}/lib/"*.dylib; do
        [ -f "$f" ] || continue
        leftover=$(non_system_deps "$f")
        if [ -n "${leftover}" ]; then
            echo "ERROR: $f still references non-system libraries:" >&2
            echo "${leftover}" >&2
            exit 1
        fi
    done

    codesign --force --sign - "$BINARY"
    for lib in "${DIST_DIR}/lib/"*.dylib; do
        [ -f "$lib" ] || continue
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
