"""Compile merge regressions against production sources and the ARM64 build.

Usage: python3 tests/test_upstream_merge.py --build BUILD --sysroot SYSROOT
Runs under qemu-aarch64-static. HIDRAW syscalls are fault-injected. The FRN
fixture replaces only ModuleFrn's configuration-provider shell; production
QsoFrn, Async sockets, timers, parsers and GSM codecs run without modification.
These tests do not require hardware, external servers, or API credentials.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build, sysroot = args.build.resolve(), args.sysroot.resolve()
    flags = (build / 'svxlink/modules/frn/CMakeFiles/ModuleFrn.dir/flags.make').read_text()
    includes = shlex.split(re.search(r'^CXX_INCLUDES = (.*)$', flags, re.M)[1])
    libdirs = [build / 'lib', sysroot / 'usr/lib/aarch64-linux-gnu', sysroot / 'lib/aarch64-linux-gnu']
    compiler = ['aarch64-linux-gnu-g++', '--sysroot=' + str(sysroot), '-std=c++11', '-O0', '-g',
                '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', '-DINTERNAL_SAMPLE_RATE=16000',
                *includes, '-I' + str(root / 'src/svxlink/modules/frn')]
    # Match CMake's direct dependency paths. Do not add the extracted sysroot
    # to -L: its libc linker scripts can refer to absent /lib paths.
    target_lib = sysroot / 'usr/lib/aarch64-linux-gnu'
    linker = ['-Wl,--gc-sections', '-L' + str(build / 'lib'),
              '-Wl,-rpath-link,' + ':'.join(str(p) for p in libdirs),
              '-lasynccpp', '-lasyncaudio', '-lasynccore', '-lresolv', str(target_lib / 'libsigc-2.0.so'),
              str(target_lib / 'libgsm.so'), '-pthread']
    with tempfile.TemporaryDirectory(prefix='svxlink-merge-tests-') as directory:
        temp = Path(directory)
        hidraw = temp / 'hidraw'
        sources = [root / 'tests/hidraw_recovery.cpp',
                   *[root / 'src/svxlink/trx' / name for name in
                     ('PttHidraw.cpp', 'SquelchHidraw.cpp', 'Squelch.cpp')]]
        subprocess.run([*compiler, *map(str, sources),
                        *['-Wl,--wrap=' + name for name in ('open', 'close', 'ioctl', 'read', 'write')],
                        *linker, '-o', str(hidraw)], check=True)
        prefix = '''#include <bits/stdc++.h>
#include <sigc++/sigc++.h>
#include <AsyncCppApplication.h>
#define private public
#define protected public
#include <AsyncTcpClient.h>
#include "QsoFrn.h"
#undef protected
#undef private
'''
        stub = '''class ModuleFrn {
  Async::Config settings;
public:
  Async::Config& cfg() { return settings; }
  std::string cfgName() { return "Frn"; }
};'''
        source = (root / 'src/svxlink/modules/frn/QsoFrn.cpp').read_text()
        assert source.count('#include "ModuleFrn.h"') == 1
        source = source.replace('#include "ModuleFrn.h"', stub)
        unit = temp / 'frn.cpp'
        unit.write_text(prefix + source + (root / 'tests/frn_merge.cpp').read_text())
        frn = temp / 'frn'
        subprocess.run([*compiler, str(unit), str(root / 'src/svxlink/modules/frn/Utils.cpp'),
                        *linker, '-o', str(frn)], check=True)
        for executable in (hidraw, frn):
            subprocess.run(['qemu-aarch64-static', '-L', str(sysroot), '-E',
                            'LD_LIBRARY_PATH=' + ':'.join(str(p) for p in libdirs), str(executable)],
                           check=True, timeout=45)


if __name__ == '__main__':
    main()
