# Prebuilt SvxLink Bundles

The ARM64 bundle is a staged SvxLink installation for 64-bit Raspberry Pi OS
(`uname -m` prints `aarch64`). It includes the executable, SvxLink libraries,
logic and module plugins, Tcl event handlers, systemd files, and other runtime
assets. Sound files are installed separately by the AIORS deployment script.

Install host-side CMake and the C++ cross-compiler once:

```sh
sudo apt-get install -y cmake g++-aarch64-linux-gnu
```

Create the isolated ARM64 dependency sysroot, then build and refresh the
Git-tracked bundle:

```sh
make sysroot-arm64
make prebuilt-arm64
```

The build uses the AIORS deployment settings: Qt disabled, `MinSizeRel`, `/usr`
prefix, `/etc` configuration, `/var` state, and systemd support. The output is:

```text
prebuilt/arm64/svxlink-rootfs.tar.gz
prebuilt/arm64/manifest.env
prebuilt/arm64/runtime-dependencies.txt
prebuilt/arm64/SHA256SUMS
```

The archive is normalized for Debian 13's usrmerged filesystem: systemd units
are installed under `/usr/lib/systemd/system`, file modes are deterministic,
and no top-level `/lib`, `/bin`, or `/sbin` tree is packaged. Because Debian 13
ships a newer JSONCPP SONAME than the build sysroot, the exact JSONCPP runtime
used by SvxLink is bundled in a private RPATH directory. SvxLink's GPIO support
is linked against Debian 13's native libgpiod v2 ABI (`libgpiod.so.3`).

The intermediate CMake build tree defaults to WSL's native cache directory,
not the Windows-mounted repository. This avoids NTFS/DrvFS timestamp skew and
permission behavior while the finished bundle is still written to
`prebuilt/arm64`. Set `SVXLINK_BUILD_DIR` only when a different native Linux
build location is required.

The public installer in `S56TS/aiors_deploy` downloads the signed combined
release, verifies this bundle, and installs it without source-repository access.
Do not deploy only `/usr/bin/svxlink`; the logic cores, modules, project
libraries, and Tcl files in the bundle are required at runtime.
