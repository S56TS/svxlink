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

The public installer in `S56TS/aiors_deploy` downloads the signed combined
release, verifies this bundle, and installs it without source-repository access.
Do not deploy only `/usr/bin/svxlink`; the logic cores, modules, project
libraries, and Tcl files in the bundle are required at runtime.
