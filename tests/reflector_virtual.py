"""Real ARM64 reflector and two current ReflectorLogic clients on loopback.

Creates disposable PKI/configuration. Exercises authentication, talkgroups,
encoded network audio in both directions, and reconnect after server restart.
"""
import argparse
import asyncio
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import numpy as np
from virtual_radio import command


async def until(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, 'Timed out waiting for reflector routing'
        await asyncio.sleep(.1)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--trace', action='store_true')
    args = parser.parse_args()
    args.build = args.build.resolve(); args.sysroot = args.sysroot.resolve()
    with tempfile.TemporaryDirectory(prefix='svx-reflector-virtual-') as directory:
        root = Path(directory)
        # Generate disposable keys natively; emulating RSA key generation
        # obscures the network test with long, variable startup latency.
        for name in ('root', 'issuer', 'server', 'a', 'b'):
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
                            'rsa_keygen_bits:2048', '-out', str(root/(name+'.key'))],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        (root/'server.conf').write_text(f'''[GLOBAL]
LISTEN_PORT={port}
CERT_PKI_DIR={root/'server-pki'}
CERT_CA_HOOK=
COMMAND_PTY={root/'server-control'}
CODECS=OPUS
[ROOT_CA]
COMMON_NAME=Local Test Root
KEYFILE={root/'root.key'}
[ISSUING_CA]
COMMON_NAME=Local Test Issuer
KEYFILE={root/'issuer.key'}
[SERVER_CERT]
COMMON_NAME=localhost
KEYFILE={root/'server.key'}
SUBJECT_ALT_NAME=IP:127.0.0.1
[USERS]
S56AA=test
S56BB=test
[PASSWORDS]
test=local-disposable-test-key
''')
        processes, readers, logs = [], [], {}
        async def launch(label, binary, config):
            invocation = command(args, binary)
            if args.trace: invocation.insert(1, '-strace')
            process = await asyncio.create_subprocess_exec(*invocation, '--config', str(config),
                      stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
            logs[label] = []
            async def read():
                while line := await process.stdout.readline():
                    logs[label].append(line.decode(errors='replace'))
            readers.append(asyncio.create_task(read())); processes.append(process)
            return process
        async def stop(process):
            if process.returncode is None: process.terminate()
            await asyncio.wait_for(process.wait(), 8)
            assert process.returncode == 0, f'Reflector test process exited {process.returncode}'
        server = await launch('server', 'svxreflector', root/'server.conf')
        try:
            await until(lambda: (root/'server-control').exists() or server.returncode is not None, 35)
            assert server.returncode is None
            (root/'events.tcl').write_text('namespace eval ReflectorLogic {}\nproc unknown {args} {}\n')
            for label, callsign, frequency, start, second in [('a','S56AA',1000,10000,24000),
                                                              ('b','S56BB',700,16000,0)]:
                node = root/label; node.mkdir()
                t = np.arange(16000) / 16000
                (node/'input.pcm').write_bytes((4000*np.sin(2*np.pi*frequency*t)).astype('<i2').tobytes())
                (node/'client.conf').write_text(f'''[GLOBAL]
LOGICS=TestLogic,ReflectorLogic
LINKS=TestLink
LOGIC_CORE_PATH={args.build/'lib'}
STATS_AIORS_ENABLE=0
STATS_PERSIST_PATH={node/'stats'}
STATS_SNAPSHOT_PATH={node/'snapshot'}
STATS_HISTORY_PATH={node/'history'}
[TestLogic]
TYPE=ChatTest
INPUT_FILE={node/'input.pcm'}
OUTPUT_FILE={node/'received.f32'}
START_MS={start}
SECOND_MS={second}
[ReflectorLogic]
TYPE=Reflector
HOSTS=127.0.0.1
HOST_PORT={port}
CALLSIGN={callsign}
AUTH_KEY=local-disposable-test-key
CERT_PKI_DIR={node/'pki'}
CERT_KEYFILE={root/(label+'.key')}
CERT_EMAIL=test@example.invalid
CERT_DOWNLOAD_CA_BUNDLE=1
EVENT_HANDLER={root/'events.tcl'}
DEFAULT_TG=42
MONITOR_TGS=42
MUTE_FIRST_TX_LOC=0
TG_SELECT_TIMEOUT=60
[TestLink]
CONNECT_LOGICS=TestLogic,ReflectorLogic
DEFAULT_ACTIVE=1
''')
                await launch(label, 'svxlink', node/'client.conf')
            await until(lambda: all('Authentication OK' in ''.join(logs[k]) for k in ('a','b')))
            print('PASS reflector: two real current clients authenticated to the real ARM64 server')
            await until(lambda: (root/'b/received.f32').stat().st_size > 16000)
            await until(lambda: (root/'a/received.f32').stat().st_size > 16000)
            print('PASS reflector: bidirectional talkgroup 42 audio through real codec/TCP/UDP paths')
            before = (root/'b/received.f32').stat().st_size
            await stop(server)
            await asyncio.sleep(.5)
            server = await launch('server-restart', 'svxreflector', root/'server.conf')
            await until(lambda: all('Disconnected from' in ''.join(logs[k]) for k in ('a','b')))
            await until(lambda: (root/'b/received.f32').stat().st_size > before + 16000, 25)
            for label, frequency in [('a',700), ('b',1000)]:
                received = np.fromfile(root/label/'received.f32', dtype='<f4')
                assert received.size > 4000 and np.sqrt(np.mean(received**2)) > .01
                spectrum = np.abs(np.fft.rfft(received[:16000]))
                peak = np.argmax(spectrum[1:]) + 1
                measured = peak * 16000 / min(len(received),16000)
                assert abs(measured-frequency) < 15, (label, measured)
            print('PASS reflector: server restart/reconnect and resumed audio; received tone frequencies verified')
        except BaseException:
            for label, lines in logs.items():
                if args.trace:
                    print(''.join(label+': '+line for line in lines if any(key in line for key in
                          ('sendto(', 'recvfrom(', 'socket(', 'bind(', 'TEST fixture', '*** ERROR'))))
                else: print(''.join([label+': '+line for line in lines])[-12000:])
            raise
        finally:
            for process in reversed(processes): await stop(process)
            await asyncio.gather(*readers)


if __name__ == '__main__': asyncio.run(main())
