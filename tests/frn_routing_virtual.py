"""Run real ModuleFrn + ChatLogic + simulated RF on loopback.

The local FRN peer grants transmit requests and exchanges real GSM WAV49.
The voice helper is deterministic; no FRN account, cloud API or radio is used.
Omit --sysroot for native execution. --soak-seconds keeps one process alive
across repeated traffic/faults and records resources with --soak-report.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import numpy as np
from chat_native import read_frame, send
from virtual_radio import command, events


def control(path, line):
    fd = os.open(path, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try: os.write(fd, line.encode())
    finally: os.close(fd)


def codec(args, root):
    source = root/'codec.cpp'
    source.write_text(r'''
#include <gsm/gsm.h>
#include <cmath>
#include <cstdio>
int main(int argc, char**) {
  gsm state = gsm_create(); int wav49=1;
  if (!state || gsm_option(state, GSM_OPT_WAV49, &wav49)<0) return 1;
  short pcm[1600]; unsigned char encoded[325];
  if (argc==1) {
    for (int n=0;n<1600;++n) pcm[n]=4000*std::sin(2*3.141592653589793*700*n/8000);
    for (int n=0;n<5;++n) {
      gsm_encode(state,pcm+n*320,encoded+n*65);
      gsm_encode(state,pcm+n*320+160,encoded+n*65+32);
    }
    std::fwrite(encoded,1,325,stdout);
  } else while (std::fread(encoded,1,325,stdin)==325) {
    for (int n=0;n<5;++n) {
      if (gsm_decode(state,encoded+n*65,pcm+n*320)<0 ||
          gsm_decode(state,encoded+n*65+33,pcm+n*320+160)<0) return 2;
    }
    std::fwrite(pcm,2,1600,stdout);
  }
  gsm_destroy(state);
}''')
    binary = root/'codec'
    compiler = ['aarch64-linux-gnu-g++', '--sysroot='+str(args.sysroot)] if args.sysroot else ['g++']
    gsm_lib = str(args.sysroot/'usr/lib/aarch64-linux-gnu/libgsm.so') if args.sysroot else '-lgsm'
    subprocess.run([*compiler, '-O2', str(source), gsm_lib,
                    '-o', str(binary)], check=True)
    invocation = command(args)[:-1] + [str(binary)]
    return invocation, subprocess.check_output(invocation)


def tone_check(samples, rate, frequency, label):
    assert len(samples) > rate*.2, (label, len(samples))
    samples = samples.astype(float)
    assert np.sqrt(np.mean(samples**2)) > .01, label
    spectrum = np.abs(np.fft.rfft(samples))
    first_voice_bin = max(1, int(300*len(samples)/rate))
    peak = np.argmax(spectrum[first_voice_bin:])+first_voice_bin
    measured = peak*rate/len(samples)
    assert abs(measured-frequency)<20, (label, measured)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--soak-seconds', type=float, default=0)
    parser.add_argument('--soak-active-rx', action='store_true',
                        help='Drop FRN during active speech and require PTT release before reconnect')
    parser.add_argument('--soak-report', type=Path)
    parser.add_argument('--soak-log', type=Path)
    parser.add_argument('--frn-debug', action='store_true')
    args = parser.parse_args()
    args.build=args.build.resolve()
    if args.sysroot: args.sysroot=args.sysroot.resolve()
    with tempfile.TemporaryDirectory(prefix='frn-radio-chat-') as directory:
        root=Path(directory)
        args.original_build=args.build
        if args.soak_seconds:
            # A rebuild must not replace a shared-library inode mapped by an
            # ongoing soak. Give this process an immutable runtime snapshot.
            from soak_support import freeze_build
            args.build=freeze_build(args.build,root)
        codec_command, packet = codec(args, root)
        assert len(packet)==325
        frn_connected=asyncio.Queue()
        frames, captured, tasks, writers = [], [], [], []
        helper_writers=[]
        stats={'frn_connections':0, 'helper_connections':0, 'frn_frames':0,
               'helper_audio_bytes':0, 'grant_tx':True, 'last_helper_events':[]}
        played=asyncio.Event()
        helper_ready=asyncio.Event()
        reply=(3500*np.sin(2*np.pi*1000*np.arange(8000)/16000)).astype('<i2').tobytes()
        async def frn(reader,writer):
            writers.append(writer)
            try:
                login=await reader.readline()
                assert login.startswith(b'CT:'), login
                writer.write(b'2014003\r\n<AL>OK</AL>\r\n'); await writer.drain()
                assert await reader.readline()==b'RX0\r\n'
                stats['frn_connections']+=1
                frn_connected.put_nowait(writer)
                while line := await reader.readline():
                    if line==b'TX0\r\n':
                        if stats['grant_tx']:
                            writer.write(b'\x01\x00\x01'); await writer.drain()
                    elif line==b'TX1\r\n':
                        frames.append(await reader.readexactly(325))
                        stats['frn_frames']+=1
                    elif line==b'RX0\r\n':
                        writer.write(b'\x00'); await writer.drain()
                    else: assert line==b'P\r\n', line
            except (asyncio.IncompleteReadError, ConnectionError): pass
        async def helper(reader,writer):
            writers.append(writer)
            helper_writers.append(writer)
            stats['helper_connections']+=1
            try:
                while True:
                    frame=await read_frame(reader)
                    if frame[:1]==b'A':
                        captured.append(frame[1:])
                        stats['helper_audio_bytes']+=len(frame)-1
                        await send(writer,b'J'+json.dumps({'event':'ack','bytes':len(frame)-1}).encode())
                        continue
                    event=json.loads(frame[1:])
                    stats['last_helper_events'].append(event)
                    del stats['last_helper_events'][:-40]
                    if event['event']=='hello':
                        await send(writer,b'J{"event":"ready"}')
                        helper_ready.set()
                    elif event['event']=='ping': await send(writer,b'J{"event":"pong"}')
                    elif event['event']=='command' and event['action']=='start':
                        await send(writer,b'P'+struct.pack('!I',event['epoch'])+reply)
                    elif event['event']=='played': played.set()
            except (asyncio.IncompleteReadError, ConnectionError): pass
        def accept(handler):
            return lambda reader,writer: tasks.append(asyncio.create_task(handler(reader,writer)))
        frn_server=await asyncio.start_server(accept(frn),'127.0.0.1',0)
        helper_server=await asyncio.start_server(accept(helper),'127.0.0.1',0)
        frn_port=frn_server.sockets[0].getsockname()[1]
        helper_port=helper_server.sockets[0].getsockname()[1]
        (root/'input.pcm').write_bytes(reply)
        (root/'events.tcl').write_text(events(root)+'''
namespace eval Frn {}
proc RepeaterLogic::activatefrn {} { injectDtmf "7#" }
proc unknown {args} {}
'''+'''proc RepeaterLogic::repeater_up {reason} {
variable repeater_is_up; set repeater_is_up 1
set f [open {%s} w]; close $f
}
''' % (root/'up')+'''proc RepeaterLogic::health {token} {
set f [open {%s} w]; puts $f $token; close $f
}
''' % (root/'health'))
        (root/'svxlink.conf').write_text(f'''[GLOBAL]
LOGICS=TestLogic,RepeaterLogic,ChatLogic
LINKS=TestLink,ChatLink
LOGIC_CORE_PATH={args.build/'lib'}
MODULE_PATH={args.build/'lib'}
CARD_SAMPLE_RATE=16000
CARD_CHANNELS=1
CARD_BLOCK_SIZE=160
STATS_AIORS_ENABLE=0
STATS_PERSIST_PATH={root/'stats'}
STATS_SNAPSHOT_PATH={root/'snapshot'}
STATS_HISTORY_PATH={root/'history'}
[TestLogic]
TYPE=VirtualRadio
INPUT_FILE={root/'input.pcm'}
OUTPUT_FILE={root/'linked.f32'}
START_MS=0
[RepeaterLogic]
TYPE=Repeater
MODULES=ModuleFrn
RX=Rx1
TX=Tx1
CALLSIGN=TEST
EVENT_HANDLER={root/'events.tcl'}
IDLE_TIMEOUT=1
OPEN_ON_SQL=0
OPEN_SQL_FLANK=OPEN
RGR_SOUND_DELAY=0
TX_CTCSS=LOGIC
COMMAND_PTY={root/'command'}
[Rx1]
TYPE=LocalSim
SQL_DET=PTY
PTY_PATH={root/'squelch'}
SIM_TONE_FQ=1000
SIM_TONE_PWR=-20
SIGLEV_DET=NONE
DEEMPHASIS=0
[Tx1]
TYPE=Local
AUDIO_DEV=virtual:{root/'transmitted.pcm'}
AUDIO_CHANNEL=0
PTT_TYPE=NONE
TX_DELAY=300
PREEMPHASIS=0
CTCSS_FQ=123
CTCSS_LEVEL=-20
[TestLink]
CONNECT_LOGICS=TestLogic,RepeaterLogic
DEFAULT_ACTIVE=1
[ChatLink]
CONNECT_LOGICS=RepeaterLogic,ChatLogic
DEFAULT_ACTIVE=1
[ChatLogic]
TYPE=Chat
SERVICE_PORT={helper_port}
TX_DELAY_MS=250
[ModuleFrn]
FRN_DEBUG={int(args.frn_debug)}
NAME=Frn
ID=7
MUTE_LOGIC_LINKING=0
SERVER=127.0.0.1
PORT={frn_port}
SERVER_BACKUP=127.0.0.1
PORT_BACKUP={frn_port}
EMAIL_ADDRESS=test@example.invalid
DYN_PASSWORD=disposable-test
CALLSIGN_AND_USER=TEST
CLIENT_TYPE=1
BAND_AND_CHANNEL=test
DESCRIPTION=local test
COUNTRY=test
CITY_CITY_PART=test
NET=test
VERSION=2014003
''')
        process=await asyncio.create_subprocess_exec(*command(args),'--config',str(root/'svxlink.conf'),
                  stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.STDOUT)
        output_task=asyncio.create_task(process.stdout.read())
        try:
            async with asyncio.timeout(40+args.soak_seconds+90*(args.soak_seconds>0)):
                await helper_ready.wait()
                control(root/'squelch','O')
                while not (root/'up').exists(): await asyncio.sleep(.02)
                control(root/'command','EVENT activatefrn\n')
                await asyncio.sleep(.1)
                control(root/'squelch','Z')
                peer=await frn_connected.get()
                await asyncio.sleep(1.5)
                captured.clear(); frames.clear()
                radio_offset=(root/'transmitted.pcm').stat().st_size
                for _ in range(4):
                    peer.write(b'\x02\x00\x01'+packet); await peer.drain()
                    await asyncio.sleep(.2)
                peer.write(b'\x00'); await peer.drain()
                await asyncio.sleep(1.5)
                tone_check(np.frombuffer(b''.join(captured),dtype='<i2')/32768,16000,700,'FRN to chat')
                radio=np.frombuffer((root/'transmitted.pcm').read_bytes()[radio_offset:],dtype='<i2')/32768
                # Include the complete burst so the final CTCSS-only tail
                # cannot hide the voice peak.
                tone_check(radio,16000,700,'FRN to RF')
                print('PASS FRN routing: real ModuleFrn WAV49 receive reaches RF and ChatLogic')
                captured.clear(); frames.clear()
                control(root/'squelch','O')
                await asyncio.sleep(.7)
                control(root/'squelch','Z')
                await asyncio.sleep(1.5)
                tone_check(np.frombuffer(b''.join(captured),dtype='<i2')/32768,16000,1000,'RF to chat')
                decoded=subprocess.check_output(codec_command+['decode'],input=b''.join(frames))
                tone_check(np.frombuffer(decoded,dtype='<i2')/32768,8000,1000,'RF to FRN')
                print('PASS FRN routing: simulated RF reaches ChatLogic and real FRN encoder/socket')
                frames.clear()
                offset=(root/'transmitted.pcm').stat().st_size
                control(root/'command','EVENT activatechat\n')
                await played.wait()
                await asyncio.sleep(.5)
                decoded=subprocess.check_output(codec_command+['decode'],input=b''.join(frames))
                tone_check(np.frombuffer(decoded,dtype='<i2')/32768,8000,1000,'Chat to FRN')
                radio=np.frombuffer((root/'transmitted.pcm').read_bytes()[offset:],dtype='<i2')/32768
                tone_check(radio,16000,1000,'Chat to RF')
                print('PASS FRN routing: chat reply reaches RF and FRN while ModuleFrn remains active')
                if args.soak_seconds:
                    from soak_support import run_soak
                    await run_soak(args,root,process,peer,packet,frn_connected,
                                   helper_ready,helper_writers,played,captured,frames,stats)
                # Exercise teardown with an active module and RF still keyed.
                control(root/'squelch','O')
                await asyncio.sleep(.7)
        except BaseException:
            if process.returncode is None: process.terminate()
            await asyncio.wait_for(process.wait(),5)
            print((await output_task).decode(errors='replace'))
            raise
        finally:
            if process.returncode is None: process.terminate()
            await asyncio.wait_for(process.wait(),5)
            output=(await output_task).decode(errors='replace')
            if args.soak_log:
                args.soak_log.parent.mkdir(parents=True,exist_ok=True)
                args.soak_log.write_text(output)
            for server in (frn_server,helper_server): server.close()
            for writer in writers: writer.close()
            results=[]
            for task in tasks:
                if task.done() and not task.cancelled() and task.exception(): results.append(task.exception())
                task.cancel()
            await asyncio.gather(*tasks,return_exceptions=True)
            for server in (frn_server,helper_server): await asyncio.wait_for(server.wait_closed(),3)
        assert not results, results
        if args.soak_report and args.soak_report.exists():
            report=json.loads(args.soak_report.read_text())
            report['shutdown_returncode']=process.returncode
            if process.returncode!=0: report['status']='failed'
            args.soak_report.write_text(json.dumps(report,indent=2)+'\n')
        assert '*** ERROR' not in output and process.returncode==0, output
        print('PASS FRN routing: clean shutdown with active FRN and open RF squelch')


if __name__=='__main__': asyncio.run(main())
