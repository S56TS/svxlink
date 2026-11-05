"""Run real RepeaterLogic/LocalTx with a paced, test-only sound card under QEMU.

Checks linked speech, transmitter start delay, generated CTCSS and its logic
mask, plus real simulated receiver squelch/PTT. No physical devices or API.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import tempfile
import time
import struct
import numpy as np
from chat_native import read_frame, send


def command(args, binary='svxlink'):
    libs = ':'.join(map(str, [args.build / 'lib', args.sysroot / 'usr/lib/aarch64-linux-gnu']))
    return ['qemu-aarch64-static', '-L', str(args.sysroot), '-E', 'LD_LIBRARY_PATH=' + libs,
            str(args.build / 'bin' / binary)]


def events(root):
    return '''namespace eval RepeaterLogic {
variable repeater_is_up 0
proc startup {} {}
proc every_second {} {}
proc every_minute {} {}
proc squelch_open {args} {}
proc siglev_updated {args} {}
proc send_rgr_sound {} {}
proc activatechat {} { publishStateEvent Chat:command "ChatLogic start" }
proc repeater_up {reason} { variable repeater_is_up; set repeater_is_up 1 }
proc repeater_down {reason} { variable repeater_is_up; set repeater_is_up 0 }
proc transmit {args} {
  set f [open {%s} a]
  puts $f "[clock microseconds] $args"
  close $f
}
}
''' % (root / 'ptt.events')


async def scenario(args, mask, rf=False, chat=False, shutdown_open=False):
    with tempfile.TemporaryDirectory(prefix='aiors-virtual-radio-') as directory:
        root = Path(directory)
        rate = 16000
        t = np.arange(rate // 2) / rate
        tone = (3500 * np.sin(2*np.pi*1000*t)).astype('<i2').tobytes()
        (root / 'input.pcm').write_bytes(tone)
        played = asyncio.Event()
        helper_tasks, helper_writers = [], []
        async def helper(reader, writer):
            helper_writers.append(writer)
            try:
                while True:
                    frame = await read_frame(reader)
                    if frame[:1] == b'A':
                        await send(writer, b'J'+json.dumps({'event':'ack','bytes':len(frame)-1}).encode())
                        continue
                    event = json.loads(frame[1:])
                    if event['event'] == 'hello': await send(writer, b'J{"event":"ready"}')
                    elif event['event'] == 'ping': await send(writer, b'J{"event":"pong"}')
                    elif event['event'] == 'command' and event['action'] == 'start':
                        await send(writer, b'P'+struct.pack('!I',event['epoch'])+tone)
                    elif event['event'] == 'played': played.set()
            except (asyncio.IncompleteReadError, ConnectionError): pass
        def accept(reader, writer): helper_tasks.append(asyncio.create_task(helper(reader,writer)))
        server = await asyncio.start_server(accept, '127.0.0.1', 0)
        port = server.sockets[0].getsockname()[1]
        (root / 'events.tcl').write_text(events(root))
        (root / 'svxlink.conf').write_text(f'''[GLOBAL]
LOGICS=TestLogic,RepeaterLogic{',ChatLogic' if chat else ''}
LINKS=TestLink{',ChatLink' if chat else ''}
LOGIC_CORE_PATH={args.build / 'lib'}
CARD_SAMPLE_RATE=16000
CARD_CHANNELS=1
CARD_BLOCK_SIZE=160
STATS_AIORS_ENABLE=0
STATS_PERSIST_PATH={root / 'stats-totals'}
STATS_SNAPSHOT_PATH={root / 'stats-snapshot'}
STATS_HISTORY_PATH={root / 'stats-history'}
[TestLogic]
TYPE=VirtualRadio
INPUT_FILE={root / 'input.pcm'}
OUTPUT_FILE={root / 'link-received.f32'}
START_MS={60000 if rf or chat else 500}
[RepeaterLogic]
TYPE=Repeater
RX=Rx1
TX=Tx1
CALLSIGN=TEST
EVENT_HANDLER={root / 'events.tcl'}
IDLE_TIMEOUT=1
OPEN_ON_SQL=0
OPEN_SQL_FLANK=OPEN
RGR_SOUND_DELAY=0
TX_CTCSS={mask}
COMMAND_PTY={root / 'command'}
[Rx1]
TYPE={'LocalSim' if rf else 'Dummy'}
SQL_DET=PTY
PTY_PATH={root / 'squelch'}
SIM_TONE_FQ=1000
SIM_TONE_PWR=-20
SIGLEV_DET=NONE
DEEMPHASIS=0
[Tx1]
TYPE=Local
AUDIO_DEV=virtual:{root / 'transmitted.pcm'}
AUDIO_CHANNEL=0
PTT_TYPE=NONE
TX_DELAY=300
PREEMPHASIS=0
CTCSS_FQ=123.0
CTCSS_LEVEL=-20
[TestLink]
CONNECT_LOGICS=TestLogic,RepeaterLogic
DEFAULT_ACTIVE=1
[ChatLogic]
TYPE=Chat
SERVICE_PORT={port}
TX_DELAY_MS=250
[ChatLink]
CONNECT_LOGICS=RepeaterLogic,ChatLogic
DEFAULT_ACTIVE=1
''')
        process = await asyncio.create_subprocess_exec(*command(args), '--config', str(root / 'svxlink.conf'),
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
        output_task = asyncio.create_task(process.stdout.read())
        try:
            if chat:
                deadline = time.monotonic() + 8
                while not (root/'command').exists():
                    assert time.monotonic() < deadline
                    await asyncio.sleep(.05)
                await asyncio.sleep(.3)
                fd = os.open(root/'command', os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
                try: os.write(fd, b'EVENT activatechat\n')
                finally: os.close(fd)
                await asyncio.wait_for(played.wait(), 8)
            if rf:
                deadline = time.monotonic() + 8
                while not (root / 'squelch').exists() and process.returncode is None:
                    assert time.monotonic() < deadline, 'No simulated RX control PTY'
                    await asyncio.sleep(.05)
                with (root / 'squelch').open('w') as squelch:
                    await asyncio.sleep(.3)
                    squelch.write('O'); squelch.flush()
                    await asyncio.sleep(.7)
                    if not shutdown_open:
                        squelch.write('Z'); squelch.flush()
            await asyncio.sleep(3.0)
        finally:
            if process.returncode is None: process.terminate()
            await asyncio.wait_for(process.wait(), 5)
            output = (await asyncio.wait_for(output_task, 3)).decode(errors='replace')
            server.close()
            for writer in helper_writers: writer.close()
            for task in helper_tasks: task.cancel()
            await asyncio.gather(*helper_tasks, return_exceptions=True)
            await asyncio.wait_for(server.wait_closed(), 3)
        try:
            assert '*** ERROR' not in output, output
            assert process.returncode == 0, f'Virtual repeater exit {process.returncode}: {output}'
            edges = [line.split() for line in (root / 'ptt.events').read_text().splitlines()]
            on = [int(edge[0]) for edge in edges if edge[-1] == '1']
            off = [int(edge[0]) for edge in edges if edge[-1] == '0']
            assert on and any(edge > on[0] for edge in off), edges
            pcm = np.fromfile(root / 'transmitted.pcm', dtype='<i2').astype(float)
            blocks = [tuple(map(int, line.split())) for line in (root / 'transmitted.pcm.blocks').read_text().splitlines()]
            voice = []
            for timestamp, offset, count in blocks:
                samples = pcm[offset:offset+count]
                amplitude = 2 * abs(np.dot(samples, np.exp(-2j*np.pi*1000*np.arange(count)/rate))) / count
                if amplitude > 400: voice.append(timestamp)
            assert voice, 'No 1 kHz speech reached the real transmitter audio chain'
            # LocalTx opens its output immediately after signalling TX-on.
            # Use the backend's monotonic clock for both timestamps: WSL can
            # adjust wall time independently while the test is running.
            opens = list(map(int, (root/'transmitted.pcm.opens').read_text().split()))
            assert len(opens) >= 2, opens  # initialization probe, then TX
            delay = (voice[0] - opens[1]) / 1000
            expected = 300 + (250 if chat else 0)
            assert expected-20 <= delay <= expected+700, f'Configured {expected} ms total delay measured {delay:.1f} ms'
            # A long window separates the subaudible CTCSS from speech leakage.
            samples = pcm[:min(len(pcm), 8000)]
            def power(frequency):
                return 2 * abs(np.dot(samples, np.exp(-2j*np.pi*frequency*np.arange(len(samples))/rate))) / len(samples)
            ctcss = power(123)
            expect_tone = mask in ('ALWAYS', 'LOGIC') or (rf and mask == 'SQL_OPEN')
            assert (ctcss > 1000) if expect_tone else (ctcss < 150), (mask, rf, ctcss)
            if rf:
                received = np.fromfile(root / 'link-received.f32', dtype='<f4')
                assert received.size > rate * .3 and np.sqrt(np.mean(received**2)) > .02
            print(f'PASS virtual radio: RF={rf}, Chat={chat}, shutdown_open={shutdown_open}, TX_CTCSS={mask}, speech delay={delay:.1f} ms, CTCSS amplitude={ctcss:.1f}')
        except BaseException:
            print(output)
            raise


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    args = parser.parse_args()
    args.build = args.build.resolve(); args.sysroot = args.sysroot.resolve()
    for mask, rf in [('LOGIC', False), ('SQL_OPEN', False), ('SQL_OPEN', True)]:
        await scenario(args, mask, rf)
    await scenario(args, 'LOGIC', chat=True)
    await scenario(args, 'SQL_OPEN', rf=True, shutdown_open=True)


if __name__ == '__main__':
    asyncio.run(main())
