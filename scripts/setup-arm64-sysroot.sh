#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
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
libglibmm-2.4-dev:arm64'

sysroot_format=3
package_key=$(printf 'format=%s\ngpiod=debian-trixie\n%s\n' "$sysroot_format" "$packages" |
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
# shellcheck disable=SC2086 # Intentional package-list splitting.
sudo apt-get -y --download-only \
  -o "Dir::Cache::archives=$download_dir/" \
  install $packages

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

# SvxLink supports libgpiod v2. Overlay only Debian 13's gpiod development and
# runtime packages so the resulting binary uses the same GPIO ABI as the Pi;
# do not replace the Ubuntu cross sysroot's libc or other toolchain libraries.
native_tmp=${TMPDIR:-/var/tmp}
debian_apt_root=${SVXLINK_DEBIAN_APT_ROOT:-"$native_tmp/svxlink-apt-debian13-arm64-$(id -u)"}
debian_deb_cache=${SVXLINK_DEBIAN_DEB_CACHE:-"$native_tmp/svxlink-debs-debian13-arm64-$(id -u)"}
debian_keyring=/usr/share/keyrings/debian-archive-keyring.gpg
[ -r "$debian_keyring" ] || {
  sudo apt-get install -y debian-archive-keyring
}
rm -rf "$debian_apt_root" "$debian_deb_cache"
mkdir -p \
  "$debian_apt_root/etc/apt/sources.list.d" \
  "$debian_apt_root/state/lists/partial" \
  "$debian_apt_root/cache/archives/partial" \
  "$debian_deb_cache"
: > "$debian_apt_root/status"
printf '%s\n' \
  "deb [arch=arm64 signed-by=$debian_keyring] https://deb.debian.org/debian trixie main" \
  > "$debian_apt_root/sources.list"

apt-get \
  -o "Dir::Etc::sourcelist=$debian_apt_root/sources.list" \
  -o "Dir::Etc::sourceparts=-" \
  -o "Dir::State=$debian_apt_root/state" \
  -o "Dir::State::status=$debian_apt_root/status" \
  -o "Dir::Cache=$debian_apt_root/cache" \
  -o "Dir::Cache::archives=$debian_deb_cache" \
  -o "APT::Architecture=arm64" \
  -o "APT::Architectures=arm64" \
  -o "Acquire::Languages=none" \
  -o "Debug::NoLocking=1" \
  update
apt-get \
  -o "Dir::Etc::sourcelist=$debian_apt_root/sources.list" \
  -o "Dir::Etc::sourceparts=-" \
  -o "Dir::State=$debian_apt_root/state" \
  -o "Dir::State::status=$debian_apt_root/status" \
  -o "Dir::Cache=$debian_apt_root/cache" \
  -o "Dir::Cache::archives=$debian_deb_cache" \
  -o "APT::Architecture=arm64" \
  -o "APT::Architectures=arm64" \
  -o "Acquire::Languages=none" \
  -o "Debug::NoLocking=1" \
  --download-only -y install libgpiod-dev:arm64

rm -f "$sysroot/usr/include/gpiod.h" "$sysroot/usr/include/gpiod.hpp"
find "$sysroot/usr/lib/aarch64-linux-gnu" -maxdepth 1 \
  -name 'libgpiod*' -delete
find "$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig" -maxdepth 1 \
  -name 'libgpiod*.pc' -delete
gpiod_dev_found=0
gpiod_runtime_found=0
for deb in "$debian_deb_cache"/*.deb; do
  [ -f "$deb" ] || continue
  case "$(dpkg-deb -f "$deb" Package)" in
    libgpiod-dev)
      dpkg-deb -x "$deb" "$sysroot"
      gpiod_dev_found=1
      ;;
    libgpiod3)
      dpkg-deb -x "$deb" "$sysroot"
      gpiod_runtime_found=1
      ;;
  esac
done
[ "$gpiod_dev_found" = 1 ] || { echo "Debian 13 libgpiod-dev was not downloaded" >&2; exit 1; }
[ "$gpiod_runtime_found" = 1 ] || { echo "Debian 13 libgpiod3 was not downloaded" >&2; exit 1; }
[ -r "$sysroot/usr/include/gpiod.h" ] || { echo "Debian 13 gpiod.h is missing" >&2; exit 1; }
[ -r "$sysroot/usr/lib/aarch64-linux-gnu/libgpiod.so.3" ] || {
  echo "Debian 13 libgpiod.so.3 is missing" >&2
  exit 1
}

printf '%s\n' "$package_key" > "$stamp"
echo "ARM64 sysroot ready: $sysroot"
