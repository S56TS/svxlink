SHELL := /bin/sh

CROSS_ARM64_PREFIX ?= aarch64-linux-gnu-
SVXLINK_SYSROOT ?= $(CURDIR)/build-cross/sysroot-arm64
CROSS_ARM64_PKG_CONFIG_LIBDIR ?= $(SVXLINK_SYSROOT)/usr/lib/aarch64-linux-gnu/pkgconfig:$(SVXLINK_SYSROOT)/usr/share/pkgconfig

.PHONY: sysroot-arm64 cross-check-arm64 cross-arm64 prebuilt-arm64 clean-cross

sysroot-arm64:
	SVXLINK_SYSROOT='$(SVXLINK_SYSROOT)' sh scripts/setup-arm64-sysroot.sh

cross-check-arm64:
	@for tool in cmake pkg-config tar sha256sum $(CROSS_ARM64_PREFIX)gcc $(CROSS_ARM64_PREFIX)g++ $(CROSS_ARM64_PREFIX)readelf; do \
		command -v "$$tool" >/dev/null || { echo "missing $$tool"; exit 1; }; \
	done
	@test "$$($(CROSS_ARM64_PREFIX)gcc -dumpmachine)" = "aarch64-linux-gnu" || { \
		echo "$(CROSS_ARM64_PREFIX)gcc does not target aarch64-linux-gnu"; exit 1; \
	}
	@test -r '$(SVXLINK_SYSROOT)/usr/include/alsa/asoundlib.h' || { \
		echo "ARM64 sysroot is missing; run: make sysroot-arm64"; exit 1; \
	}
	@test -r '$(SVXLINK_SYSROOT)/usr/include/gpiod.h' || { \
		echo "ARM64 sysroot is missing libgpiod; run: make sysroot-arm64"; exit 1; \
	}
	@test -r '$(SVXLINK_SYSROOT)/usr/lib/aarch64-linux-gnu/libgpiod.a' || { \
		echo "ARM64 sysroot is missing static libgpiod; run: make sysroot-arm64"; exit 1; \
	}
	@PKG_CONFIG_LIBDIR='$(CROSS_ARM64_PKG_CONFIG_LIBDIR)' pkg-config --exists \
		alsa sigc++-2.0 jsoncpp libcurl openssl libgpiod || { \
		echo "missing one or more ARM64 development packages: alsa sigc++-2.0 jsoncpp libcurl openssl libgpiod"; \
		exit 1; \
	}

cross-arm64: cross-check-arm64
	CROSS_ARM64_PREFIX='$(CROSS_ARM64_PREFIX)' \
	CROSS_ARM64_PKG_CONFIG_LIBDIR='$(CROSS_ARM64_PKG_CONFIG_LIBDIR)' \
	SVXLINK_SYSROOT='$(SVXLINK_SYSROOT)' \
	sh scripts/cross-build-arm64.sh build

prebuilt-arm64: cross-check-arm64
	CROSS_ARM64_PREFIX='$(CROSS_ARM64_PREFIX)' \
	CROSS_ARM64_PKG_CONFIG_LIBDIR='$(CROSS_ARM64_PKG_CONFIG_LIBDIR)' \
	SVXLINK_SYSROOT='$(SVXLINK_SYSROOT)' \
	sh scripts/cross-build-arm64.sh bundle

clean-cross:
	cmake -E remove_directory build-cross
