#!/usr/bin/env bash
# Build the turnip-a8xx Vulkan driver and package it.
#
# Usage:  a8xx/build.sh [--deps] [--target linux|android] [release-name]
#   --deps          install build dependencies first (Ubuntu 24.04 x86_64, sudo)
#   --target linux  (default) aarch64 glibc Linux build, for DroidDeck and other
#                   glibc Linux-on-Android setups
#   --target android  Android (bionic) build packaged for AdrenoTools, for
#                   GameHub, GameNative and other apps that load AdrenoTools
#                   driver packages; needs the Android NDK (ANDROID_NDK_HOME,
#                   or downloaded by --deps)
#   release-name    name used in the zip, e.g. r55 (default: git describe)
#
# Output (last line printed is the zip path):
#   linux:   out/Turnip-<ver>-a8xx-<name>-linux.zip
#            folder Turnip-…-linux/ with libvulkan_freedreno.so, icd.json, README.txt
#   android: out/Turnip-<ver>-a8xx-<name>-adrenotools.zip
#            meta.json + libvulkan_freedreno.so at the top level
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

DEPS=0
TARGET=linux
NAME=
while [[ $# -gt 0 ]]; do
   case "$1" in
   --deps) DEPS=1 ;;
   --target) TARGET="$2"; shift ;;
   --target=*) TARGET="${1#--target=}" ;;
   *) NAME="$1" ;;
   esac
   shift
done
[[ "$TARGET" == linux || "$TARGET" == android ]] || { echo "unknown target $TARGET" >&2; exit 1; }

NDK_VERSION=android-ndk-r29
SDK_VERSION=34

install_common_deps() {
   sudo apt-get update
   sudo apt-get install -y --no-install-recommends \
      pkg-config ninja-build bison flex glslang-tools python3-pip python3-mako \
      python3-yaml python3-packaging libwayland-bin zip unzip curl git
   pip3 install --user --break-system-packages 'meson>=1.4.0'
   export PATH="$HOME/.local/bin:$PATH"
}

install_linux_deps() {
   sudo dpkg --add-architecture arm64
   # Ubuntu serves arm64 packages from ports.ubuntu.com; restrict the existing
   # sources to amd64 and add a ports source for arm64.
   if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]] &&
      ! grep -q '^Architectures:' /etc/apt/sources.list.d/ubuntu.sources; then
      sudo sed -i '/^Types: deb/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources
   fi
   . /etc/os-release
   sudo tee /etc/apt/sources.list.d/arm64-ports.list >/dev/null <<EOF
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${VERSION_CODENAME} main universe
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${VERSION_CODENAME}-updates main universe
EOF
   install_common_deps
   sudo apt-get install -y --no-install-recommends \
      gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
      libdrm-dev:arm64 libexpat1-dev:arm64 zlib1g-dev:arm64 libzstd-dev:arm64 \
      libwayland-dev:arm64 libx11-dev:arm64 libx11-xcb-dev:arm64 \
      libxcb1-dev:arm64 libxcb-dri3-dev:arm64 libxcb-present-dev:arm64 \
      libxcb-randr0-dev:arm64 libxcb-shm0-dev:arm64 libxcb-sync-dev:arm64 \
      libxcb-xfixes0-dev:arm64 libxrandr-dev:arm64 libxshmfence-dev:arm64
   # Mesa needs a newer wayland-protocols than Ubuntu 24.04 has, and its
   # subproject fallback wants a newer wayland-scanner than wayland-client.
   # wayland-protocols is only XML + a .pc file: install those directly.
   if ! pkg-config --atleast-version=1.45 wayland-protocols; then
      rm -rf /tmp/wp-src
      git clone -q --depth 1 -b 1.45 https://gitlab.freedesktop.org/wayland/wayland-protocols.git /tmp/wp-src
      sudo mkdir -p /usr/local/share/wayland-protocols /usr/local/share/pkgconfig
      for d in stable staging unstable experimental; do
         [[ -d /tmp/wp-src/$d ]] && sudo cp -r /tmp/wp-src/$d /usr/local/share/wayland-protocols/
      done
      sudo tee /usr/local/share/pkgconfig/wayland-protocols.pc >/dev/null <<'PC'
prefix=/usr/local
datarootdir=${prefix}/share
pkgdatadir=${datarootdir}/wayland-protocols

Name: Wayland Protocols
Description: Wayland protocol files
Version: 1.45
PC
   fi
}

install_android_deps() {
   install_common_deps
   sudo apt-get install -y --no-install-recommends clang lld llvm
   if [[ -z "${ANDROID_NDK_HOME:-}" || ! -d "${ANDROID_NDK_HOME}" ]]; then
      if [[ ! -d "/tmp/$NDK_VERSION" ]]; then
         curl -sL "https://dl.google.com/android/repository/${NDK_VERSION}-linux.zip" -o /tmp/ndk.zip
         unzip -q /tmp/ndk.zip -d /tmp
         rm -f /tmp/ndk.zip
      fi
      export ANDROID_NDK_HOME="/tmp/$NDK_VERSION"
   fi
}

if [[ $DEPS == 1 ]]; then
   if [[ $TARGET == linux ]]; then install_linux_deps; else install_android_deps; fi
fi
export PATH="$HOME/.local/bin:$PATH"

NAME="${NAME:-$(git describe --tags --always 2>/dev/null || echo dev)}"
VERSION="$(cat VERSION)"

build_linux() {
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

   PKG="Turnip-${VERSION}-a8xx-${NAME}-linux"
   STAGE="$ROOT/out/$PKG"
   rm -rf "$STAGE" "$ROOT/out/$PKG.zip"
   mkdir -p "$STAGE"
   cp "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so" "$STAGE/"
   cp a8xx/icd.json "$STAGE/icd.json"
   cp a8xx/BUILD-NOTES.txt "$STAGE/README.txt"
   (cd "$ROOT/out" && zip -qr "$PKG.zip" "$PKG")
   echo "$ROOT/out/$PKG.zip"
}

build_android() {
   : "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME or use --deps}"
   local ndk="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
   BUILD="${BUILD_DIR:-$ROOT/build-a8xx-android}"
   mkdir -p "$BUILD"

   # Newer NDK headers are stricter about Android's native_handle constness.
   # Patch the Android-only files for the build and restore them afterwards.
   local fixed=(include/android_stub/cutils/native_handle.h
                src/util/u_gralloc/u_gralloc_fallback.c
                src/vulkan/runtime/vk_android.c)
   trap 'git checkout -q -- "${fixed[@]}" 2>/dev/null || true' EXIT
   sed -i 's/typedef const native_handle_t\* buffer_handle_t;/typedef void\* buffer_handle_t;/g' include/android_stub/cutils/native_handle.h
   sed -i 's/, hnd->handle/, (void \*)hnd->handle/g' src/util/u_gralloc/u_gralloc_fallback.c
   sed -i -E 's/([a-z_]+)->handle->/((const native_handle_t *)\1->handle)->/g' src/vulkan/runtime/vk_android.c

   cat > "$BUILD/android-aarch64.txt" <<EOF
[binaries]
ar = '$ndk/llvm-ar'
c = ['$ndk/aarch64-linux-android${SDK_VERSION}-clang']
cpp = ['$ndk/aarch64-linux-android${SDK_VERSION}-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = '$ndk/ld.lld'
cpp_ld = '$ndk/ld.lld'
strip = '$ndk/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$ndk/pkg-config', '/usr/bin/pkg-config']

[built-in options]
c_args = ['-D__ANDROID__', '-Wno-error', '-Wno-deprecated-declarations', '-Wno-incompatible-pointer-types-discards-qualifiers', '-Wno-incompatible-pointer-types']
cpp_args = ['-D__ANDROID__', '-Wno-error', '-Wno-deprecated-declarations', '-Wno-incompatible-pointer-types-discards-qualifiers', '-Wno-incompatible-pointer-types']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
   cat > "$BUILD/native.txt" <<EOF
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
c_ld = 'lld'
cpp_ld = 'lld'
EOF
   if [[ ! -f "$BUILD/build.ninja" ]]; then
      meson setup "$BUILD" \
         --cross-file "$BUILD/android-aarch64.txt" \
         --native-file "$BUILD/native.txt" \
         -Dbuildtype=release -Db_ndebug=true -Dstrip=true \
         -Dplatforms=android -Dplatform-sdk-version=${SDK_VERSION} \
         -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
         -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dvulkan-beta=true \
         -Dfreedreno-kmds=kgsl -Degl=disabled -Dvideo-codecs= \
         -Dallow-fallback-for=libdrm
   fi
   ninja -C "$BUILD" src/freedreno/vulkan/libvulkan_freedreno.so

   local vk_patch vk_minor
   vk_patch=$(grep '^#define VK_HEADER_VERSION ' include/vulkan/vulkan_core.h | awk '{print $3}')
   vk_minor=$(grep -oP 'define TU_API_VERSION VK_MAKE_VERSION\(\s*[0-9]+,\s*\K[0-9]+' src/freedreno/vulkan/tu_device.cc || echo 4)

   local mitig="TU_SAFEMODE on by default"
   grep -q 'TU_SAFEMODE=0 turns it off' src/freedreno/vulkan/tu_util.cc ||
      mitig="mitigations off by default in this older build"

   PKG="Turnip-${VERSION}-a8xx-${NAME}-adrenotools"
   STAGE="$ROOT/out/$PKG"
   rm -rf "$STAGE" "$ROOT/out/$PKG.zip"
   mkdir -p "$STAGE"
   cp "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so" "$STAGE/"
   cat > "$STAGE/meta.json" <<EOF
{
  "schemaVersion": 1,
  "name": "Turnip a8xx ${NAME}",
  "description": "Mesa Turnip ${VERSION} with Adreno 8xx GPC-fault mitigations (${mitig}). github.com/kyudongkim/turnip-a8xx",
  "author": "kyudongkim",
  "packageVersion": "1",
  "vendor": "Mesa",
  "driverVersion": "Vulkan 1.${vk_minor}.${vk_patch}",
  "minApi": 28,
  "libraryName": "libvulkan_freedreno.so"
}
EOF
   (cd "$STAGE" && zip -q "$ROOT/out/$PKG.zip" libvulkan_freedreno.so meta.json)
   echo "$ROOT/out/$PKG.zip"
}

if [[ $TARGET == linux ]]; then build_linux; else build_android; fi
