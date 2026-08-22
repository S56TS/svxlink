#!/usr/bin/env sh
set -eu

mode=${1:-bundle}
case "$mode" in
  build|bundle) ;;
  *) echo "usage: $0 [build|bundle]" >&2; exit 2 ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${SVXLINK_BUILD_DIR:-"$repo_dir/build-cross/arm64"}
stage_dir="$build_dir/stage"
prebuilt_dir=${SVXLINK_PREBUILT_DIR:-"$repo_dir/prebuilt/arm64"}
toolchain="$repo_dir/cmake/toolchains/raspberrypi-arm64.cmake"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')}
cross_prefix=${CROSS_ARM64_PREFIX:-aarch64-linux-gnu-}
pkgconfig_libdir=${CROSS_ARM64_PKG_CONFIG_LIBDIR:-/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig}
source_commit=$(git -C "$repo_dir" rev-parse HEAD)
source_describe=$(git -C "$repo_dir" describe --tags --always 2>/dev/null || printf '%s' "$source_commit")
source_dirty=0
if ! git -C "$repo_dir" diff --quiet HEAD -- . ':(exclude)prebuilt/**'; then
  source_dirty=1
fi
if git -C "$repo_dir" ls-files --others --exclude-standard |
    grep -Ev '^(build-cross|prebuilt)/' | grep -q .; then
  source_dirty=1
fi

cmake_args=""
if [ -n "${SVXLINK_SYSROOT:-}" ]; then
  cmake_args="$cmake_args -DSVXLINK_SYSROOT=${SVXLINK_SYSROOT}"
  cmake_args="$cmake_args -DGPIOD_LIBRARY=${SVXLINK_SYSROOT}/usr/lib/aarch64-linux-gnu/libgpiod.a"
fi

chmod +x "$repo_dir/scripts/pkg-config-arm64.sh"

PKG_CONFIG_PATH= \
PKG_CONFIG_LIBDIR="$pkgconfig_libdir" \
cmake -S "$repo_dir/src" -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DPKG_CONFIG_EXECUTABLE="$repo_dir/scripts/pkg-config-arm64.sh" \
  -DCROSS_ARM64_PREFIX="$cross_prefix" \
  -DCROSS_ARM64_PKG_CONFIG_LIBDIR="$pkgconfig_libdir" \
  -DUSE_QT=OFF \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=lib/aarch64-linux-gnu \
  -DSYSCONF_INSTALL_DIR=/etc \
  -DLOCAL_STATE_DIR=/var \
  -DWITH_SYSTEMD=YES \
  $cmake_args

if grep -E '(:FILEPATH|:PATH)=.*(/usr/lib/x86_64-linux-gnu|/usr/include/x86_64-linux-gnu)' \
    "$build_dir/CMakeCache.txt" >/dev/null; then
  echo "cross-build resolved an x86-64 library or include path:" >&2
  grep -E '(:FILEPATH|:PATH)=.*(/usr/lib/x86_64-linux-gnu|/usr/include/x86_64-linux-gnu)' \
    "$build_dir/CMakeCache.txt" >&2
  exit 1
fi

cmake --build "$build_dir" --parallel "$jobs"
[ "$mode" = bundle ] || exit 0

cmake -E remove_directory "$stage_dir"
cmake -E make_directory "$stage_dir" "$prebuilt_dir"
DESTDIR="$stage_dir" cmake --install "$build_dir"

readelf_tool="${cross_prefix}readelf"
runtime_deps_tmp="$build_dir/runtime-dependencies.txt.tmp"
: > "$runtime_deps_tmp"
find "$stage_dir" -type f -print | while IFS= read -r filename; do
  if "$readelf_tool" -h "$filename" >/dev/null 2>&1; then
    machine=$("$readelf_tool" -h "$filename" | awk -F: '/Machine:/ { gsub(/^[[:space:]]+/, "", $2); print $2; exit }')
    case "$machine" in
      AArch64) ;;
      *) echo "unexpected ELF architecture '$machine': $filename" >&2; exit 1 ;;
    esac
    "$readelf_tool" -d "$filename" 2>/dev/null |
      sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' >> "$runtime_deps_tmp"
  fi
done
sort -u "$runtime_deps_tmp" > "$prebuilt_dir/runtime-dependencies.txt"
rm -f "$runtime_deps_tmp"

bundle="$prebuilt_dir/svxlink-rootfs.tar.gz"
bundle_tmp="$bundle.tmp"
tar --sort=name --mtime='@0' --numeric-owner --owner=0 --group=0 \
  -C "$stage_dir" -czf "$bundle_tmp" .
mv "$bundle_tmp" "$bundle"

bundle_sha256=$(sha256sum "$bundle" | awk '{print $1}')

cat > "$prebuilt_dir/manifest.env" <<EOF
ARTIFACT_FORMAT=1
ARCH=arm64
GNU_TRIPLET=aarch64-linux-gnu
BUNDLE=svxlink-rootfs.tar.gz
BUNDLE_SHA256=$bundle_sha256
SOURCE_COMMIT=$source_commit
SOURCE_DESCRIBE=$source_describe
SOURCE_DIRTY=$source_dirty
CMAKE_BUILD_TYPE=MinSizeRel
CMAKE_INSTALL_PREFIX=/usr
CMAKE_INSTALL_LIBDIR=lib/aarch64-linux-gnu
SYSCONF_INSTALL_DIR=/etc
LOCAL_STATE_DIR=/var
WITH_SYSTEMD=YES
USE_QT=OFF
EOF

(cd "$prebuilt_dir" && sha256sum svxlink-rootfs.tar.gz > SHA256SUMS)
echo "ARM64 bundle ready: $bundle"
