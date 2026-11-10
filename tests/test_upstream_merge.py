"""Compile merge regressions against production sources and the ARM64 build.

Usage: python3 tests/test_upstream_merge.py --build BUILD --sysroot SYSROOT
Omit --sysroot for native execution; add --sanitizers for a native instrumented
build. ARM64 binaries run under qemu-aarch64-static. HIDRAW syscalls are fault-injected. The FRN
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
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--sanitizers', action='store_true', help='Native ASan/UBSan build')
    parser.add_argument('--only', choices=('all', 'timing', 'frn'), default='all')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    sysroot = args.sysroot.resolve() if args.sysroot else None
    flags = (build / 'svxlink/modules/frn/CMakeFiles/ModuleFrn.dir/flags.make').read_text()
    includes = shlex.split(re.search(r'^CXX_INCLUDES = (.*)$', flags, re.M)[1])
    libdirs = [build/'lib']
    if sysroot: libdirs += [sysroot/'usr/lib/aarch64-linux-gnu', sysroot/'lib/aarch64-linux-gnu']
    compiler = (['aarch64-linux-gnu-g++', '--sysroot='+str(sysroot)] if sysroot else ['g++']) + [
                '-std=c++11', '-O0', '-g',
                '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', '-DINTERNAL_SAMPLE_RATE=16000',
                *includes, '-I' + str(root / 'src/svxlink/modules/frn'),
                '-I' + str(root / 'src/async/audio')]
    # Match CMake's direct dependency paths. Do not add the extracted sysroot
    # to -L: its libc linker scripts can refer to absent /lib paths.
    multiarch = 'aarch64-linux-gnu' if sysroot else subprocess.check_output(
        ['g++','-print-multiarch'],text=True).strip()
    target_lib = (sysroot or Path('/'))/'usr/lib'/multiarch
    linker = ['-Wl,--gc-sections', '-L' + str(build / 'lib'),
              '-Wl,-rpath-link,' + ':'.join(str(p) for p in libdirs),
              '-lasynccpp', '-lasyncaudio', '-lasynccore', '-lresolv', str(target_lib / 'libsigc-2.0.so'),
              str(target_lib / 'libgsm.so'), '-pthread']
    if args.sanitizers:
        assert not sysroot, 'Use native execution for AddressSanitizer/LeakSanitizer'
        sanitizer_flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
        compiler += sanitizer_flags
        linker += sanitizer_flags
    def execute(executable, *arguments, timeout=45):
        prefix = ['qemu-aarch64-static', '-L', str(sysroot), '-E'] if sysroot else ['env']
        subprocess.run([*prefix, 'LD_LIBRARY_PATH='+':'.join(map(str,libdirs)),
                        str(executable), *map(str,arguments)], check=True, timeout=timeout)
    def run_timing(temp):
        timing = temp / 'tx-timing'
        subprocess.run([*compiler, str(root / 'tests/tx_timing.cpp'),
                        str(root / 'src/svxlink/trx/PttCtrl.cpp'),
                        *linker, '-o', str(timing)], check=True)
        for delay in (0, 100, 300, 600):
            execute(timing, delay, timeout=8)
    with tempfile.TemporaryDirectory(prefix='svxlink-merge-tests-') as directory:
        temp = Path(directory)
        if args.only == 'timing':
            run_timing(temp)
            return
        hidraw = temp / 'hidraw'
        sources = [root / 'tests/hidraw_recovery.cpp',
                   *[root / 'src/svxlink/trx' / name for name in
                     ('PttHidraw.cpp', 'SquelchHidraw.cpp', 'Squelch.cpp')]]
        if args.sanitizers:
            # UBSan's vptr metadata retains references from the unused generic
            # factory to every unrelated squelch implementation. Keep this
            # isolated fixture small; the full native application build still
            # instruments Squelch.cpp with vptr checking enabled.
            sql_object=temp/'squelch.o'
            subprocess.run([*compiler,'-fno-sanitize=vptr','-c',str(sources.pop()),
                            '-o',str(sql_object)],check=True)
            sources.append(sql_object)
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
        if args.only == 'frn':
            execute(frn)
            return
        framed = temp/'framed'
        subprocess.run([*compiler, str(root/'tests/framed_reconnect.cpp'),
                        *linker,'-o',str(framed)],check=True)
        lifetime=temp/'timer-lifetime'
        subprocess.run([*compiler,str(root/'tests/timer_lifetime.cpp'),
                        *linker,'-o',str(lifetime)],check=True)
        for executable in (hidraw, frn, framed, lifetime):
            execute(executable)
        san=temp/'ssl-san'
        crypto_libs = ([str(target_lib/'libssl.so'),str(target_lib/'libcrypto.so')]
                       if sysroot else ['-lssl','-lcrypto'])
        subprocess.run([*compiler,str(root/'tests/ssl_san.cpp'),*linker,*crypto_libs,
                        '-o',str(san)],check=True)
        execute(san)
        run_timing(temp)


if __name__ == '__main__':
    main()
