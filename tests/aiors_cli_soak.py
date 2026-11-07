"""Long-run real AIORS CLI/LED bridge plus full SvxLink diagnostics collection.

The companion checkout is read only. Compile its actual CLI, aiorsctl and
support sources into a private temporary root; only GPIO is stubbed and uhubctl
is a harmless delayed shell script. No installed services or hardware are used.
Pass --sysroot for ARM64/QEMU, or --sanitizers with a native sanitizer build.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time


def sample(pid):
    result = {}
    try:
        lines = Path(f'/proc/{pid}/status').read_text().splitlines()
    except (FileNotFoundError, ProcessLookupError):
        return None
    for line in lines:
        key, _, value = line.partition(':')
        if key in ('VmRSS', 'VmHWM', 'Threads'):
            result[key] = int(value.split()[0])
    # waitpid/poll and /proc are not an atomic snapshot. A normally exiting
    # child can become a zombie after the loop condition and lose its VmRSS.
    if not all(key in result for key in ('VmRSS','VmHWM','Threads')):
        return None
    try:
        result['fds'] = len(list(Path(f'/proc/{pid}/fd').iterdir()))
    except (FileNotFoundError, ProcessLookupError):
        return None
    return result


def resources(samples, duration):
    samples = [s for s in samples if 'VmRSS' in s]
    warmup = min(120, duration / 3)
    stable = [s for s in samples if s['seconds'] >= warmup]
    assert stable, 'No resource samples after warmup'
    count = max(1, min(60, len(stable) // 3))
    first, last = stable[:count], stable[-count:]
    result = dict(warmup_seconds=warmup, samples=len(samples),
                  peak_rss_kib=max(s['VmRSS'] for s in samples),
                  max_fds=max(s['fds'] for s in samples),
                  max_threads=max(s['Threads'] for s in samples))
    for key in ('VmRSS', 'fds', 'Threads'):
        result[key + '_growth'] = (statistics.median(s[key] for s in last) -
                                  statistics.median(s[key] for s in first))
    assert result['VmRSS_growth'] < 16384, result
    assert result['fds_growth'] <= 2, result
    assert result['Threads_growth'] <= 0, result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bsp', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--sanitizers', action='store_true')
    parser.add_argument('--asan-quarantine-mib', type=int, default=16,
                        help='ASan freed-memory quarantine; zero helps isolate allocator overhead')
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--outage-ms', type=int, default=2300,
                        help='CLI downtime per restart; 7500 guarantees diagnostics-client timeouts')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    assert args.seconds >= 20
    assert args.outage_ms > 2000
    assert args.asan_quarantine_mib >= 0
    assert not (args.sanitizers and args.sysroot)
    repo = Path(__file__).resolve().parents[1]
    bsp, build = args.bsp.resolve(), args.build.resolve()
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    result = {'status': 'running', 'requested_seconds': args.seconds,
              'mode': 'arm64-qemu' if args.sysroot else 'native-sanitizers' if args.sanitizers else 'native',
              'bsp_commit': subprocess.check_output(['git', '-C', str(bsp), 'rev-parse', 'HEAD'], text=True).strip(),
              'svxlink_commit': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
              'asan_quarantine_mib': args.asan_quarantine_mib if args.sanitizers else None,
              'bridge_samples': [], 'svxlink_samples': []}
    with tempfile.TemporaryDirectory(prefix='aiors-cli-soak-') as directory:
        root = Path(directory)
        cmd = root / 'cmd'
        # Freeze application/libs; compiling or rebuilding elsewhere cannot
        # overwrite a mapped runtime during this test.
        runtime = root / 'runtime'
        shutil.copytree(build / 'lib', runtime / 'lib', symlinks=True)
        (runtime / 'bin').mkdir()
        shutil.copy2(build / 'bin/svxlink', runtime / 'bin/svxlink')
        flags = ['-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                 '-DGPIO_FORCE_STUB', f'-DCMD_DIR="{cmd}"',
                 f'-DCTL_CMD_DIR="{cmd}"', '-I'+str(bsp/'include'), '-I'+str(bsp/'src')]
        san = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all'] if args.sanitizers else []
        cc = ['aarch64-linux-gnu-gcc', '--sysroot='+str(args.sysroot)] if args.sysroot else ['gcc']
        cxx = ['aarch64-linux-gnu-g++', '--sysroot='+str(args.sysroot)] if args.sysroot else ['g++']
        names = 'cfg cli gpio i2c uart usb cm108 radio_type radio_sa818 radio_sa858 radio_ext util log routing'.split()
        def compile_source(name):
            subprocess.run([*cc, '-std=gnu11', *flags, *san, '-c', str(bsp/'src'/f'{name}.c'),
                            '-o', str(root/f'{name}.o')], check=True)
        print('Compiling actual CLI/GPIO/LED worker and aiorsctl:', result['mode'], flush=True)
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile_source, names))
        bridge = root / 'cli-bridge'
        subprocess.run([*cxx, '-std=c++11', *flags, *san, '-I'+str(repo/'src/svxlink/svxlink'),
                        str(repo/'tests/aiors_cli_soak.cpp'),
                        str(repo/'src/svxlink/svxlink/AiorsLedClient.cpp'),
                        *map(str, sorted(root.glob('*.o'))), '-lm', '-o', str(bridge)], check=True)
        # QEMU user emulation cannot exec an ARM64 helper without host binfmt
        # registration. Use the actual unmodified CLI client compiled for host;
        # the daemon, worker and SvxLink remain ARM64 in that mode.
        ctl = root / 'aiorsctl'
        subprocess.run(['gcc', '-std=gnu11', *flags, *san, str(bsp/'src/ctl.c'),
                        str(bsp/'src/util.c'), '-o', str(ctl)], check=True)
        helper = root / 'uhubctl'
        helper.write_text('#!/bin/sh\nsleep 1\necho "Port 2: power on"\n')
        helper.chmod(0o700)
        env = dict(os.environ, PATH=str(root)+':'+os.environ['PATH'])
        if args.sanitizers:
            env['ASAN_OPTIONS'] = ('detect_leaks=1:halt_on_error=1:abort_on_error=1:quarantine_size_mb='
                                   + str(args.asan_quarantine_mib))
            env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        libraries = [runtime / 'lib']
        if args.sysroot:
            libraries += [args.sysroot/'usr/lib/aarch64-linux-gnu', args.sysroot/'lib/aarch64-linux-gnu']
        env['LD_LIBRARY_PATH'] = ':'.join(map(str, libraries))
        def execute(path):
            if args.sysroot:
                return ['qemu-aarch64-static', '-L', str(args.sysroot), str(path)]
            return [str(path)]
        # A native helper must not inherit ARM64 library search paths.
        ctl_wrapper = root / 'collect-diagnostics'
        ctl_wrapper.write_text('#!/bin/sh\nunset LD_LIBRARY_PATH\nexec '+str(ctl)+' "$@"\n')
        ctl_wrapper.chmod(0o700)
        (root/'events.tcl').write_text('''namespace eval SimplexLogic {
proc startup {} {}
proc every_second {} {}
proc every_minute {} {}
proc transmit {args} {}
proc squelch_open {args} {}
proc health {token} {
  set f [open {%s} w]; puts $f $token; close $f
}
}
''' % (root/'health'))
        (root/'svxlink.conf').write_text(f'''[GLOBAL]
LOGICS=SimplexLogic
LOGIC_CORE_PATH={runtime/'lib'}
STATS_INTERVAL=2
STATS_AIORS_ENABLE=1
STATS_AIORSCTL_PATH={ctl_wrapper}
STATS_AIORS_SAMPLE_INTERVAL=5
STATS_LOG_TO_STDOUT=0
STATS_PERSIST_PATH={root/'stats-totals'}
STATS_SNAPSHOT_PATH={root/'stats-snapshot'}
STATS_HISTORY_PATH={root/'stats-history'}
[SimplexLogic]
TYPE=Simplex
RX=Rx1
TX=Tx1
CALLSIGN=TEST
EVENT_HANDLER={root/'events.tcl'}
COMMAND_PTY={root/'command'}
[Rx1]
TYPE=Dummy
[Tx1]
TYPE=Dummy
''')
        paths = [bridge, ctl, runtime/'bin/svxlink', runtime/'lib/libasynccore.so',
                 runtime/'lib/libasynccpp.so', repo/'src/svxlink/svxlink/AiorsLedClient.cpp', bsp/'src/cli.c']
        result['sha256'] = {str(p.relative_to(root)) if p.is_relative_to(root) else str(p):
                            hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
        bridge_log = args.report.with_suffix('.bridge.log')
        svx_log = args.report.with_suffix('.svxlink.log')
        started = time.monotonic()
        bridge_process = svx_process = None
        checks = 0
        max_health_ms = 0
        with bridge_log.open('w') as blog, svx_log.open('w') as slog:
            try:
                bridge_process = subprocess.Popen([*execute(bridge), str(args.seconds), str(args.outage_ms)],
                                                  env=env, stdout=blog, stderr=subprocess.STDOUT)
                while not cmd.exists():
                    assert bridge_process.poll() is None, bridge_log.read_text()
                    assert time.monotonic()-started < 10
                    time.sleep(.05)
                svx_process = subprocess.Popen([*execute(runtime/'bin/svxlink'), '--config', str(root/'svxlink.conf')],
                                               env=env, stdout=slog, stderr=subprocess.STDOUT)
                while not (root/'command').exists():
                    assert svx_process.poll() is None, svx_log.read_text()
                    assert time.monotonic()-started < 15
                    time.sleep(.05)
                last_progress = started
                while bridge_process.poll() is None:
                    assert svx_process.poll() is None, svx_log.read_text()
                    assert time.monotonic()-started < args.seconds+90, 'Bridge workload stalled'
                    tick = time.monotonic()
                    token = 'cli'+str(checks)
                    fd = os.open(root/'command', os.O_WRONLY|os.O_NOCTTY|os.O_NONBLOCK)
                    try:
                        os.write(fd, ('EVENT health '+token+'\n').encode())
                    finally:
                        os.close(fd)
                    while not ((root/'health').exists() and (root/'health').read_text().strip() == token):
                        assert time.monotonic()-tick < 2, 'SvxLink event loop unresponsive'
                        time.sleep(.01)
                    max_health_ms = max(max_health_ms, (time.monotonic()-tick)*1000)
                    checks += 1
                    for proc, key in ((bridge_process, 'bridge_samples'), (svx_process, 'svxlink_samples')):
                        measurement = sample(proc.pid)
                        if measurement is not None:
                            result[key].append(dict(seconds=time.monotonic()-started, **measurement))
                    # Save ongoing measurements even if a later assertion fails.
                    if time.monotonic()-last_progress >= 30:
                        last_progress = time.monotonic()
                        args.report.write_text(json.dumps(result, indent=2)+'\n')
                        print(f"CLI soak {last_progress-started:.0f}s: health={checks}, "
                              f"bridge RSS={result['bridge_samples'][-1]['VmRSS']} KiB, "
                              f"SvxLink RSS={result['svxlink_samples'][-1]['VmRSS']} KiB", flush=True)
                    time.sleep(1)
                assert bridge_process.returncode == 0, bridge_log.read_text()[-12000:]
                svx_process.terminate()
                assert svx_process.wait(timeout=10) == 0, svx_log.read_text()[-12000:]
                for path in (bridge_log, svx_log):
                    text = path.read_text()
                    assert not any(term in text for term in ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'runtime error:')), path
                line = next(line for line in bridge_log.read_text().splitlines() if line.startswith('CLI_RESULT '))
                result['workload'] = json.loads(line.split(' ', 1)[1])
                assert result['workload']['restarts'] and result['workload']['expired']
                for key in ('bridge', 'svxlink'):
                    result[key+'_resources'] = resources(result[key+'_samples'], args.seconds)
                snapshot = (root/'stats-snapshot').read_text()
                fields = dict(token.split('=',1) for token in snapshot.split() if '=' in token)
                assert int(fields['aiors_samples_ok_total']) >= max(2, args.seconds//10), fields
                assert fields['aiors_diagnostics_version'] == '5', fields
                if args.outage_ms >= 7000:
                    assert int(fields['aiors_samples_failed_total']) > 0, fields
                    assert fields['aiors_sample_ok'] == '1' and fields['aiors_sample_error'] == 'none', fields
                result['diagnostics'] = {key: value for key, value in fields.items() if key in
                    ('aiors_samples_ok_total','aiors_samples_failed_total','aiors_diagnostics_version',
                     'aiors_sample_ok','aiors_sample_error')}
                result.update(status='passed', bridge_exit=bridge_process.returncode,
                              svxlink_exit=svx_process.returncode, health_checks=checks,
                              max_health_latency_ms=max_health_ms)
            except BaseException as error:
                result.update(status='failed', error=repr(error))
                raise
            finally:
                for proc in (svx_process, bridge_process):
                    if proc is not None and proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            proc.kill(); proc.wait(timeout=5)
                result['bridge_exit'] = bridge_process.returncode if bridge_process else None
                result['svxlink_exit'] = svx_process.returncode if svx_process else None
                result['health_checks'] = checks
                result['max_health_latency_ms'] = max_health_ms
                result['elapsed_seconds'] = time.monotonic()-started
                args.report.write_text(json.dumps(result, indent=2)+'\n')
        print('PASS CLI soak:', result['mode'], result['workload'], result['diagnostics'], flush=True)


if __name__ == '__main__':
    main()
