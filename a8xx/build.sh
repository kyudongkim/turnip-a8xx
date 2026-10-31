#!/usr/bin/env bash
# Build the turnip-a8xx Vulkan driver (aarch64 Linux, glibc) and package it
# as a zip that DroidDeck (and other glibc Linux-on-Android setups) can import.
#
# Usage:  a8xx/build.sh [--deps] [release-name]
#   --deps        install build dependencies first (Ubuntu 24.04 x86_64, sudo)
#   release-name  name used in the zip, e.g. r55 (default: git describe)
#
# Output: out/Turnip-<mesa version>-a8xx-<release-name>.zip
#   libvulkan_freedreno.so, icd.json, README.txt (a8xx/BUILD-NOTES.txt)
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

if [[ "${1:-}" == "--deps" ]]; then
   shift
   sudo dpkg --add-architecture arm64
   # Ubuntu serves arm64 packages from ports.ubuntu.com; restrict the existing
   # sources to amd64 and add a ports source for arm64.
   if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
      sudo sed -i '/^Types: deb/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources
   fi
   . /etc/os-release
   sudo tee /etc/apt/sources.list.d/arm64-ports.list >/dev/null <<EOF
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${VERSION_CODENAME} main universe
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${VERSION_CODENAME}-updates main universe
EOF
   sudo apt-get update
   sudo apt-get install -y --no-install-recommends \
      gcc-aarch64-linux-gnu g++-aarch64-linux-gnu pkg-config \
      ninja-build bison flex glslang-tools python3-pip python3-mako \
      python3-yaml python3-packaging libwayland-bin zip \
      libdrm-dev:arm64 libexpat1-dev:arm64 zlib1g-dev:arm64 libzstd-dev:arm64 \
      libwayland-dev:arm64 libx11-dev:arm64 libx11-xcb-dev:arm64 \
      libxcb1-dev:arm64 libxcb-dri3-dev:arm64 libxcb-present-dev:arm64 \
      libxcb-randr0-dev:arm64 libxcb-shm0-dev:arm64 libxcb-sync-dev:arm64 \
      libxcb-xfixes0-dev:arm64 libxrandr-dev:arm64 libxshmfence-dev:arm64
   sudo apt-get install -y --no-install-recommends git libexpat1-dev
   pip3 install --user --break-system-packages 'meson>=1.4.0'
   export PATH="$HOME/.local/bin:$PATH"
   # Mesa's wayland-protocols fallback needs a build-machine wayland-scanner
   # >= 1.23; Ubuntu 24.04 ships 1.22, so build just the scanner.
   if ! pkg-config --atleast-version=1.23 wayland-scanner; then
      rm -rf /tmp/wayland-src
      git clone -q --depth 1 -b 1.23.1 https://gitlab.freedesktop.org/wayland/wayland.git /tmp/wayland-src
      meson setup /tmp/wayland-src/build /tmp/wayland-src --prefix=/usr/local \
         -Dlibraries=false -Ddocumentation=false -Ddtd_validation=false -Dtests=false
      ninja -C /tmp/wayland-src/build
      sudo env "PATH=$PATH" meson install -C /tmp/wayland-src/build
   fi
fi

NAME="${1:-$(git describe --tags --always 2>/dev/null || echo dev)}"
VERSION="$(cat VERSION)"
BUILD="${BUILD_DIR:-$ROOT/build-a8xx}"
CROSS="${CROSS_FILE:-a8xx/cross-aarch64-ubuntu.txt}"

if [[ ! -f "$BUILD/build.ninja" ]]; then
   meson setup "$BUILD" \
      --cross-file "$CROSS" \
      -Dbuildtype=release -Db_ndebug=true \
      -Dvulkan-drivers=freedreno -Dgallium-drivers= \
      -Dfreedreno-kmds=kgsl,msm -Dplatforms=x11,wayland \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
      -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
      -Dzstd=enabled -Dxmlconfig=enabled -Dvulkan-layers= -Dtools=
fi
ninja -C "$BUILD" src/freedreno/vulkan/libvulkan_freedreno.so

PKG="Turnip-${VERSION}-a8xx-${NAME}"
STAGE="$ROOT/out/$PKG"
rm -rf "$STAGE" "$ROOT/out/$PKG.zip"
mkdir -p "$STAGE"
cp "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so" "$STAGE/"
cp a8xx/icd.json "$STAGE/icd.json"
cp a8xx/BUILD-NOTES.txt "$STAGE/README.txt"
(cd "$ROOT/out" && zip -qr "$PKG.zip" "$PKG")
echo "$ROOT/out/$PKG.zip"
