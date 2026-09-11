#!/usr/bin/env sh
set -eu

mode=${1:-bundle}
case "$mode" in
  build|bundle) ;;
  *) echo "usage: $0 [build|bundle]" >&2; exit 2 ;;
esac

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
repo_key=$(printf '%s' "$repo_dir" | sha256sum | cut -c1-12)
cache_root=${XDG_CACHE_HOME:-${HOME:-/var/tmp}/.cache}
build_dir=${SVXLINK_BUILD_DIR:-"$cache_root/svxlink-cross/$repo_key/arm64"}
stage_dir="$build_dir/stage"
prebuilt_dir=${SVXLINK_PREBUILT_DIR:-"$repo_dir/prebuilt/arm64"}
toolchain="$repo_dir/cmake/toolchains/raspberrypi-arm64.cmake"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')}
cross_prefix=${CROSS_ARM64_PREFIX:-aarch64-linux-gnu-}
readelf_tool="${cross_prefix}readelf"
runtime_libdir=lib/aarch64-linux-gnu/svxlink/runtime
pkgconfig_libdir=${CROSS_ARM64_PKG_CONFIG_LIBDIR:-/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig}
git_repo() {
  git -c core.autocrlf=true -c core.filemode=false -C "$repo_dir" "$@"
}

source_commit=$(git_repo rev-parse HEAD)
source_describe=$(git_repo describe --tags --always 2>/dev/null || printf '%s' "$source_commit")
source_dirty=0
if ! git_repo diff --quiet HEAD -- . ':(exclude)prebuilt/**'; then
  source_dirty=1
fi
if git_repo ls-files --others --exclude-standard |
    grep -Ev '^(build-cross|prebuilt)/' | grep -q .; then
  source_dirty=1
fi

cmake_args=""
if [ -n "${SVXLINK_SYSROOT:-}" ]; then
  cmake_args="$cmake_args -DSVXLINK_SYSROOT=${SVXLINK_SYSROOT}"
fi

chmod +x "$repo_dir/scripts/pkg-config-arm64.sh"

# cmake_args is intentionally split into zero or more complete -D arguments.
# shellcheck disable=SC2086
PKG_CONFIG_PATH='' \
PKG_CONFIG_LIBDIR="$pkgconfig_libdir" \
cmake -S "$repo_dir/src" -B "$build_dir" \
  -U GPIOD_INCLUDE_DIR \
  -U GPIOD_LIBRARY \
  -U 'PC_GPIOD_*' \
  -U 'pkgcfg_lib_PC_GPIOD_*' \
  -U '__pkg_config_*PC_GPIOD*' \
  -U FIND_PACKAGE_MESSAGE_DETAILS_GPIOD \
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
  -DSYSTEMD_CONFIGURATIONS_FILES_DIR=/usr/lib/systemd/system \
  -DCMAKE_INSTALL_RPATH=/usr/$runtime_libdir \
  $cmake_args

if grep -E '(:FILEPATH|:PATH)=.*(/usr/lib/x86_64-linux-gnu|/usr/include/x86_64-linux-gnu)' \
    "$build_dir/CMakeCache.txt" >/dev/null; then
  echo "cross-build resolved an x86-64 library or include path:" >&2
  grep -E '(:FILEPATH|:PATH)=.*(/usr/lib/x86_64-linux-gnu|/usr/include/x86_64-linux-gnu)' \
    "$build_dir/CMakeCache.txt" >&2
  exit 1
fi

gpiod_version=$(sed -n 's/^PC_GPIOD_VERSION:INTERNAL=//p' "$build_dir/CMakeCache.txt" | head -n 1)
case "$gpiod_version" in
  2.*) ;;
  *) echo "cross-build must use Debian 13 libgpiod v2, found '$gpiod_version'" >&2; exit 1 ;;
esac
gpiod_library=$(sed -n 's/^GPIOD_LIBRARY:FILEPATH=//p' "$build_dir/CMakeCache.txt" | head -n 1)
[ -r "$gpiod_library" ] || { echo "cross-build libgpiod library is missing" >&2; exit 1; }
gpiod_soname=$($readelf_tool -d "$gpiod_library" |
  sed -n 's/.*Library soname: \[\([^]]*\)\].*/\1/p' | head -n 1)
[ "$gpiod_soname" = libgpiod.so.3 ] || {
  echo "cross-build must link Debian 13 libgpiod.so.3, found '$gpiod_soname'" >&2
  exit 1
}

cmake --build "$build_dir" --parallel "$jobs"
[ "$mode" = bundle ] || exit 0

cmake -E remove_directory "$stage_dir"
cmake -E make_directory "$stage_dir" "$prebuilt_dir"
DESTDIR="$stage_dir" cmake --install "$build_dir"

# Debian 13 is usrmerged. Keep the archive from carrying a real top-level
# /lib directory that could replace /lib -> usr/lib during manual extraction.
if [ -d "$stage_dir/lib/systemd" ]; then
  [ ! -e "$stage_dir/usr/lib/systemd" ] || {
    echo "both legacy and usrmerged systemd install directories were staged" >&2
    exit 1
  }
  mkdir -p "$stage_dir/usr/lib"
  mv "$stage_dir/lib/systemd" "$stage_dir/usr/lib/systemd"
  rmdir "$stage_dir/lib" || {
    echo "unexpected content remains in the staged top-level /lib" >&2
    exit 1
  }
fi
[ ! -e "$stage_dir/lib" ] || {
  echo "refusing to package a top-level /lib directory for a usrmerged target" >&2
  exit 1
}

jsoncpp_search_dir=${SVXLINK_SYSROOT:-}/usr/lib/aarch64-linux-gnu
[ -n "${SVXLINK_SYSROOT:-}" ] || {
  echo "SVXLINK_SYSROOT is required when bundling the jsoncpp runtime" >&2
  exit 1
}
jsoncpp_real=$(find "$jsoncpp_search_dir" -maxdepth 1 -type f \
  -name 'libjsoncpp.so.*' | sort | head -n 1)
[ -n "$jsoncpp_real" ] || {
  echo "could not find the jsoncpp runtime in $jsoncpp_search_dir" >&2
  exit 1
}
jsoncpp_soname=$($readelf_tool -d "$jsoncpp_real" |
  sed -n 's/.*Library soname: \[\([^]]*\)\].*/\1/p' | head -n 1)
[ -n "$jsoncpp_soname" ] || {
  echo "could not determine the jsoncpp SONAME from $jsoncpp_real" >&2
  exit 1
}
runtime_dir="$stage_dir/usr/$runtime_libdir"
mkdir -p "$runtime_dir"
install -m 0644 "$jsoncpp_real" "$runtime_dir/$(basename "$jsoncpp_real")"
ln -s "$(basename "$jsoncpp_real")" "$runtime_dir/$jsoncpp_soname"

jsoncpp_copyright=$(find "${SVXLINK_SYSROOT}/usr/share/doc" -maxdepth 2 \
  -path '*/libjsoncpp*/copyright' -type f | sort | head -n 1)
if [ -n "$jsoncpp_copyright" ]; then
  install -D -m 0644 "$jsoncpp_copyright" \
    "$stage_dir/usr/share/doc/svxlink/jsoncpp-copyright"
fi

jsoncpp_users_tmp="$build_dir/jsoncpp-users.txt.tmp"
: > "$jsoncpp_users_tmp"
find "$stage_dir" -type f -print | while IFS= read -r filename; do
  if "$readelf_tool" -d "$filename" 2>/dev/null |
      grep -q "Shared library: \[$jsoncpp_soname\]"; then
    printf '%s\n' "$filename" >> "$jsoncpp_users_tmp"
    "$readelf_tool" -d "$filename" 2>/dev/null |
      grep -q "/usr/$runtime_libdir" || {
        echo "jsoncpp consumer has no private runtime path: $filename" >&2
        exit 1
      }
  fi
done
[ -s "$jsoncpp_users_tmp" ] || {
  echo "no staged ELF file uses the bundled $jsoncpp_soname" >&2
  exit 1
}
rm -f "$jsoncpp_users_tmp"

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

package_stage=$(mktemp -d "${TMPDIR:-/tmp}/svxlink-arm64-package.XXXXXX")
cleanup_package_stage() {
  rm -rf "$package_stage"
}
trap cleanup_package_stage EXIT HUP INT TERM
cp -a "$stage_dir/." "$package_stage/"

# Builds commonly run on a Windows DrvFS mount where every file appears as
# mode 0777. Normalize release modes on a native Linux temporary filesystem.
find "$package_stage" -type d -exec chmod 0755 {} +
find "$package_stage" -type f -exec chmod 0644 {} +
find "$package_stage/usr/bin" "$package_stage/usr/sbin" \
  -type f -exec chmod 0755 {} +
if [ -f "$package_stage/usr/share/svxlink/ca-hook.py" ]; then
  chmod 0755 "$package_stage/usr/share/svxlink/ca-hook.py"
fi
if find "$package_stage" -type f -perm /022 -print | grep -q .; then
  echo "release payload contains a group/world-writable file" >&2
  exit 1
fi
if find "$package_stage/usr/lib/systemd/system" -type f -perm /111 -print |
    grep -q .; then
  echo "release payload contains an executable systemd unit" >&2
  exit 1
fi

bundle="$prebuilt_dir/svxlink-rootfs.tar.gz"
bundle_tmp="$bundle.tmp"
tar --sort=name --mtime='@0' --numeric-owner --owner=0 --group=0 \
  -C "$package_stage" -czf "$bundle_tmp" .
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
SYSTEMD_UNIT_DIR=/usr/lib/systemd/system
USE_QT=OFF
GPIOD_VERSION=$gpiod_version
GPIOD_SONAME=$gpiod_soname
BUNDLED_JSONCPP=$jsoncpp_soname
BUNDLED_RUNTIME_DIR=/usr/$runtime_libdir
EOF

(cd "$prebuilt_dir" && sha256sum svxlink-rootfs.tar.gz > SHA256SUMS)
echo "ARM64 bundle ready: $bundle"
