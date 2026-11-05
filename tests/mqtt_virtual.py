"""Compile the companion AIORS MQTT agent and test it against a local MQTT peer.

This is an integration test of the real agent, SQLite outbox and libmosquitto.
The Python peer implements only MQTT 3.1.1 CONNECT/PUBLISH/PUBACK/PING needed
here; it is not a production broker or a TLS/ACL interoperability test.
"""
import argparse
import asyncio
import json
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time


async def packet(reader):
    first = (await reader.readexactly(1))[0]
    size, multiplier = 0, 1
    for _ in range(4):
        digit = (await reader.readexactly(1))[0]
        size += (digit & 127) * multiplier
        if not digit & 128: return first, await reader.readexactly(size)
        multiplier *= 128
    raise AssertionError('Invalid MQTT remaining length')


async def wait_for(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, 'Timed out waiting for agent/broker state'
        await asyncio.sleep(.1)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bsp', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    args = parser.parse_args()
    bsp, sysroot = args.bsp.resolve(), args.sysroot.resolve()
    repo = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='aiors-mqtt-virtual-') as directory:
        root = Path(directory)
        snapshot = root / 'snapshot'
        subprocess.run(['python3', str(repo/'tests/test_svxstats_radios.py'), '--snapshot', str(snapshot)], check=True)
        target_lib = sysroot / 'usr/lib/aarch64-linux-gnu'
        agent = root / 'aiors-mqtt-agent'
        subprocess.run(['aarch64-linux-gnu-gcc', '--sysroot='+str(sysroot), '-std=gnu11', '-O2',
                        '-I'+str(bsp/'include'), str(bsp/'src/mqtt_agent.c'), str(bsp/'src/mqtt_agent_core.c'),
                        '-Wl,-rpath-link,'+str(target_lib),
                        *[str(target_lib/name) for name in ('libmosquitto.so', 'libsqlite3.so', 'libcjson.so')],
                        '-o', str(agent)], check=True)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        (root/'svxlink.conf').write_text('[GLOBAL]\nLOGICS=RepeaterLogic\n[RepeaterLogic]\nCALLSIGN=S56TEST\n')
        (root/'agent.conf').write_text(f'''[agent]
ENABLED=1
NODE_ID=S56TEST
SVXLINK_CONFIG={root/'svxlink.conf'}
SNAPSHOT_PATH={snapshot}
STATE_DB={root/'queue.db'}
AIORSCTL_PATH=/bin/false
POLL_INTERVAL_MS=250
MAX_QUEUE_MESSAGES=100
MAX_QUEUE_BYTES=4194304
[broker]
HOST=127.0.0.1
PORT={port}
TOPIC_PREFIX=frn/aiors/v1
KEEPALIVE_S=10
TLS=0
[commands]
ENABLED=0
TIMEOUT_MS=1000
''')
        loader = target_lib/'ld-linux-aarch64.so.1'
        process = await asyncio.create_subprocess_exec('qemu-aarch64-static', str(loader), '--library-path',
                    str(target_lib), str(agent), '-c', str(root/'agent.conf'),
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
        output = asyncio.create_task(process.stdout.read())
        seen, errors, writers = [], [], set()
        acknowledge = True
        connections = 0
        async def client(reader, writer):
            nonlocal connections
            writers.add(writer); connections += 1
            try:
                while True:
                    header, body = await packet(reader)
                    kind = header >> 4
                    if kind == 1:
                        assert b'MQTT' in body
                        writer.write(b'\x20\x02\x00\x00')
                    elif kind == 3:
                        length = struct.unpack('!H', body[:2])[0]
                        topic = body[2:2+length].decode(); pos = 2 + length
                        qos = (header >> 1) & 3
                        mid = body[pos:pos+2] if qos else b''
                        pos += 2 if qos else 0
                        payload = json.loads(body[pos:])
                        seen.append((topic, payload, qos, bool(header & 1)))
                        if qos == 1 and acknowledge: writer.write(b'\x40\x02'+mid)
                    elif kind == 12: writer.write(b'\xd0\x00')
                    elif kind == 14: break
                    else: raise AssertionError(f'Unexpected MQTT packet {kind}')
                    await writer.drain()
            except (asyncio.IncompleteReadError, ConnectionError): pass
            except BaseException as error: errors.append(repr(error))
            finally:
                writers.discard(writer); writer.close()
        server = None
        def rows():
            if not (root/'queue.db').exists(): return []
            with sqlite3.connect(root/'queue.db') as db:
                try: return db.execute('SELECT topic,payload FROM outbox').fetchall()
                except sqlite3.OperationalError: return []
        def telemetry(): return [item for item in seen if item[0].endswith('/telemetry')]
        try:
            await wait_for(lambda: len(rows()) >= 1)
            assert process.returncode is None
            print('PASS MQTT: broker unavailable, real agent remains alive and queues telemetry in SQLite')
            server = await asyncio.start_server(client, '127.0.0.1', port)
            await wait_for(lambda: bool(telemetry()))
            topic, payload, qos, retained = telemetry()[-1]
            assert topic == 'frn/aiors/v1/S56TEST/telemetry' and qos == 1 and retained
            assert payload['node_id'] == 'S56TEST'
            metrics = payload['metrics']
            assert len(metrics) >= 361 and metrics['stats_schema'] == 3
            for key in ('ai', 'bi', 'ae', 'be'):
                assert isinstance(metrics[f'aiors_radio_{key}_status'], str)
            await wait_for(lambda: not rows())
            print(f'PASS MQTT: reconnect, QoS 1 retained telemetry, {len(metrics)} metrics, PUBACK removes durable rows')
            # Withhold PUBACK on an updated snapshot, then interrupt transport.
            acknowledge = False
            previous = len(telemetry())
            text = snapshot.read_text()
            import re
            text = re.sub(r'ts_unix_s=(\d+)', lambda m: 'ts_unix_s='+str(int(m[1])+1), text)
            snapshot.write_text(text)
            await wait_for(lambda: len(telemetry()) > previous)
            assert rows(), 'Unacknowledged telemetry was removed prematurely'
            for writer in list(writers): writer.close()
            acknowledge = True
            previous_connections = connections
            await wait_for(lambda: connections > previous_connections)
            await wait_for(lambda: not rows())
            assert not errors, errors
            print('PASS MQTT: unacknowledged publish survives disconnect and drains after reconnect/PUBACK')
        finally:
            if process.returncode is None: process.terminate()
            await asyncio.wait_for(process.wait(), 8)
            log = (await output).decode(errors='replace')
            if process.returncode not in (0, -15): print(log)
            if server: server.close()
            for writer in list(writers): writer.close()
            if server: await asyncio.wait_for(server.wait_closed(), 3)
        assert process.returncode in (0, -15), log


if __name__ == '__main__':
    asyncio.run(main())
