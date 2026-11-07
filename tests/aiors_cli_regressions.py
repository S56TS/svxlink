"""Focused real-BSP CLI regressions and a negative control for the old leak.

No files in the BSP checkout are changed. The negative-control binary restores
only the pre-4f165e5 reply-expiration condition in a temporary cli.c copy, then
must fail the BSP's own cleanup assertion. All current implementations must pass.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bsp', type=Path, required=True)
    parser.add_argument('--sanitizers', action='store_true')
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    assert not (args.sysroot and args.sanitizers)
    bsp = args.bsp.resolve()
    bsp_commit = subprocess.check_output(['git','-C',str(bsp),'rev-parse','HEAD'],text=True).strip()
    repo = Path(__file__).resolve().parents[1]
    results = []
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='aiors-cli-regressions-') as directory:
        root = Path(directory)
        helper = root/'uhubctl'
        helper.write_text('#!/bin/sh\nsleep 1\necho "Port 2: power on"\n')
        helper.chmod(0o700)
        env = dict(os.environ, PATH=str(root)+':'+os.environ['PATH'])
        if args.sanitizers:
            env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1:abort_on_error=1:quarantine_size_mb=16'
            env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        cc = ['aarch64-linux-gnu-gcc', '--sysroot='+str(args.sysroot)] if args.sysroot else ['gcc']
        cxx = ['aarch64-linux-gnu-g++', '--sysroot='+str(args.sysroot)] if args.sysroot else ['g++']
        san = ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-sanitize-recover=all'] if args.sanitizers else []
        prefix = ['qemu-aarch64-static','-L',str(args.sysroot)] if args.sysroot else []
        for hardware in ('100', '110'):
            output = root/hardware
            output.mkdir()
            cmd = output/'cmd'
            flags = ['-O2','-g','-Wall','-Wextra','-Werror','-pthread',*san,
                     '-DGPIO_FORCE_STUB','-DAIORS_HW_VERSION='+hardware,
                     '-DAIORS_HW_VERSION_STRING="'+('1.0' if hardware=='100' else '1.1')+'"',
                     '-DAIORS_GIT_VERSION="'+bsp_commit[:8]+'"',
                     f'-DCMD_DIR="{cmd}"','-I'+str(bsp/'include'),'-I'+str(bsp/'src')]
            names = 'cfg cli gpio i2c uart usb cm108 radio_type radio_sa818 radio_sa858 radio_ext util log routing'.split()
            def compile_source(name):
                subprocess.run([*cc,'-std=gnu11',*flags,'-c',str(bsp/'src'/f'{name}.c'),
                                '-o',str(output/f'{name}.o')],check=True)
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(compile_source,names))
            objects = sorted(output.glob('*.o'))
            def execute(name, negative=False):
                run = subprocess.run([*prefix,str(output/name)],env=env,text=True,
                                     stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
                if negative:
                    assert run.returncode != 0 and 'access(stale, F_OK) != 0' in run.stdout, run.stdout
                    print('PASS negative control: pre-fix live-client reply remains and cleanup assertion rejects it',flush=True)
                else:
                    assert run.returncode == 0, run.stdout
                    assert not any(term in run.stdout for term in ('ERROR: AddressSanitizer','ERROR: LeakSanitizer','runtime error:')), run.stdout
                    print(run.stdout.strip(),flush=True)
                results.append(dict(test=name,hardware=hardware,exit=run.returncode,
                                    expected_failure=negative,output=run.stdout))
            for name in ('test_cli_reply_cleanup','test_cli_responsiveness'):
                subprocess.run([*cc,'-std=gnu11',*flags,str(bsp/'tests'/f'{name}.c'),
                                *map(str,objects),'-lm','-o',str(output/name)],check=True)
                execute(name)
            subprocess.run([*cxx,'-std=c++11',*flags,'-I'+str(repo/'src/svxlink/svxlink'),
                            str(repo/'tests/aiors_cli_boundaries.cpp'),
                            str(repo/'src/svxlink/svxlink/AiorsLedClient.cpp'),
                            *map(str,objects),'-Wl,--wrap=clock_gettime','-lm','-o',str(output/'boundaries')],check=True)
            execute('boundaries')
            source = (bsp/'src/cli.c').read_text()
            current = '''if(!cli_process_is_alive(owner_pid) ||
                   (cli_request_is_older_than(request_ms, CLI_ARTIFACT_MAX_AGE_MS) &&
                    !SA818_IsRssiReportForDest(path_entry)))'''
            assert source.count(current)==1
            legacy = output/'cli-legacy.c'
            legacy.write_text(source.replace(current,'if(!cli_process_is_alive(owner_pid))'))
            subprocess.run([*cc,'-std=gnu11',*flags,'-c',str(legacy),'-o',str(output/'cli-legacy.o')],check=True)
            subprocess.run([*cc,'-std=gnu11',*flags,str(bsp/'tests/test_cli_reply_cleanup.c'),
                            *map(str,[p for p in objects if p.name!='cli.o']),str(output/'cli-legacy.o'),
                            '-lm','-o',str(output/'legacy-cleanup')],check=True)
            execute('legacy-cleanup',negative=True)
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(dict(status='passed',bsp_commit=bsp_commit,sanitizers=args.sanitizers,
                                          mode='arm64-qemu' if args.sysroot else 'native',tests=results),indent=2)+'\n')


if __name__ == '__main__':
    main()
