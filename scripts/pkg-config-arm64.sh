#!/usr/bin/env sh
set -eu

: "${SVXLINK_SYSROOT:?SVXLINK_SYSROOT is required}"
export PKG_CONFIG_DIR=
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR="$SVXLINK_SYSROOT"
export PKG_CONFIG_LIBDIR="${CROSS_ARM64_PKG_CONFIG_LIBDIR:-$SVXLINK_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SVXLINK_SYSROOT/usr/share/pkgconfig}"
exec /usr/bin/pkg-config "$@"

