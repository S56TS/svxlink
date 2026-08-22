#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
sysroot=${SVXLINK_SYSROOT:-"$repo_dir/build-cross/sysroot-arm64"}
download_dir=${SVXLINK_DEB_CACHE:-"$repo_dir/build-cross/arm64-debs"}
stamp="$sysroot/.svxlink-arm64-sysroot"

packages='libasound2-dev:arm64
libpopt-dev:arm64
libgcrypt20-dev:arm64
libgsm1-dev:arm64
libspeex-dev:arm64
libopus-dev:arm64
libogg-dev:arm64
libjsoncpp-dev:arm64
libcurl4-openssl-dev:arm64
libssl-dev:arm64
tcl8.6-dev:arm64
libsigc++-2.0-dev:arm64
libglibmm-2.4-dev:arm64
libgpiod-dev:arm64'

sysroot_format=2
package_key=$(printf 'format=%s\n%s\n' "$sysroot_format" "$packages" |
  sha256sum | awk '{print $1}')
if [ -r "$stamp" ] && grep -qx "$package_key" "$stamp"; then
  echo "ARM64 sysroot is current: $sysroot"
  exit 0
fi

command -v dpkg-deb >/dev/null || { echo "missing dpkg-deb" >&2; exit 1; }
mkdir -p "$download_dir/partial" "$sysroot"
if [ "$(dpkg --print-architecture)" != arm64 ] &&
   ! dpkg --print-foreign-architectures | grep -qx arm64; then
  sudo dpkg --add-architecture arm64
fi
sudo apt-get update
# Download-only mode resolves dependencies but performs no package install or
# removal in WSL. Only ARM64 and architecture-independent archives are then
# extracted into the private sysroot.
sudo apt-get -y --download-only \
  -o "Dir::Cache::archives=$download_dir/" \
  install $packages

# apt-get skips archives that are already installed as foreign-architecture
# packages in WSL. Force the direct GPIO package into this private cache. If
# the repository is temporarily unavailable, reuse the installed target files.
gpiod_from_host=0
if ! (
  cd "$download_dir"
  apt-get download libgpiod-dev:arm64
); then
  dpkg-query -W libgpiod-dev:arm64 >/dev/null 2>&1 || exit 1
  gpiod_from_host=1
fi

found=0
for deb in "$download_dir"/*.deb; do
  [ -f "$deb" ] || continue
  arch=$(dpkg-deb -f "$deb" Architecture)
  case "$arch" in
    arm64|all)
      dpkg-deb -x "$deb" "$sysroot"
      found=1
      ;;
  esac
done
[ "$found" = 1 ] || { echo "no ARM64 packages were downloaded" >&2; exit 1; }

if [ "$gpiod_from_host" = 1 ]; then
  mkdir -p "$sysroot/usr/include" \
    "$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig"
  cp -a /usr/include/gpiod.h /usr/include/gpiod.hpp "$sysroot/usr/include/"
  cp -a /usr/lib/aarch64-linux-gnu/libgpiod.a \
    /usr/lib/aarch64-linux-gnu/libgpiodcxx.a \
    "$sysroot/usr/lib/aarch64-linux-gnu/"
  cp -a /usr/lib/aarch64-linux-gnu/pkgconfig/libgpiod.pc \
    /usr/lib/aarch64-linux-gnu/pkgconfig/libgpiodcxx.pc \
    "$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig/"
fi

printf '%s\n' "$package_key" > "$stamp"
echo "ARM64 sysroot ready: $sysroot"
