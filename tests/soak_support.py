"""Continuous FRN/RF/chat workload and Linux process resource monitoring.

Used by frn_routing_virtual.py --soak-seconds; the SvxLink process is never
restarted between cycles. Both native sanitizer and ARM64/QEMU runs work.
"""
import asyncio
import json
import hashlib
import os
from pathlib import Path
import statistics
import shutil
import time
import numpy as np


def freeze_build(build,root):
    """Copy a stable build before starting; later rebuilds cannot affect it."""
    runtime=root/'runtime'
    shutil.copytree(build/'lib',runtime/'lib',symlinks=True)
    assert all(path.resolve().is_relative_to(runtime) for path in (runtime/'lib').iterdir())
    (runtime/'bin').mkdir()
    for name in ('svxlink','svxreflector'):
        if (build/'bin'/name).exists(): shutil.copy2(build/'bin'/name,runtime/'bin'/name)
    return runtime


def control(path, text):
    fd=os.open(path,os.O_WRONLY|os.O_NOCTTY|os.O_NONBLOCK)
    try: os.write(fd,text.encode())
    finally: os.close(fd)


async def run_soak(args,root,process,peer,packet,connections,helper_ready,
                   helper_writers,played,captured,frames,stats):
    started=time.monotonic()
    deadline=started+args.soak_seconds
    finished=asyncio.Event()
    samples=[]
    counters={'cycles':0,'frn_midframe_disconnects':0,'helper_midframe_disconnects':0,
              'active_rx_disconnects':0,'max_ptt_release_ms':0,
              'grant_timeouts':0,'health_checks':0,'max_health_latency_ms':0,
              'max_reply_latency_ms':0,'max_reconnect_latency_ms':0}
    result={'status':'running','requested_seconds':args.soak_seconds,
            'pid':process.pid,'mode':'arm64-qemu' if args.sysroot else 'native',
            'build':str(args.original_build),'counters':counters,'samples':samples,
            'runtime_sha256':{str(path.relative_to(args.build)):hashlib.sha256(path.read_bytes()).hexdigest()
                for path in [args.build/'bin/svxlink',
                    *[args.build/'lib'/name for name in ('libasynccore.so','libasynccpp.so','libasyncaudio.so',
                                                        'RepeaterLogic.so','ModuleFrn.so','ChatLogic.so')]]}}
    # Bootstrap defines the health event before SvxLink starts. Its reply
    # proves the interpreter/event loop dispatched each probe.
    async def wait(predicate, timeout=5):
        limit=time.monotonic()+timeout
        while not predicate():
            assert process.returncode is None, f'SvxLink exited {process.returncode}'
            assert time.monotonic()<limit,'Soak operation timed out'
            await asyncio.sleep(.02)
    def audio_ok(chunks):
        if not chunks: return False
        audio=np.frombuffer(b''.join(chunks),dtype='<i2').astype(float)
        return audio.size>1600 and np.sqrt(np.mean(audio**2))>100
    async def reconnect():
        start=time.monotonic()
        next_peer=await asyncio.wait_for(connections.get(),20)
        counters['max_reconnect_latency_ms']=max(counters['max_reconnect_latency_ms'],
                                                 (time.monotonic()-start)*1000)
        await asyncio.sleep(.2)
        return next_peer
    async def monitor():
        sequence=0
        while not finished.is_set():
            assert process.returncode is None, f'SvxLink exited {process.returncode}'
            sequence+=1
            tick=time.monotonic()
            token='soak'+str(sequence)
            control(root/'command','EVENT health '+token+'\n')
            await wait(lambda: (root/'health').exists() and (root/'health').read_text().strip()==token,2)
            latency=(time.monotonic()-tick)*1000
            counters['health_checks']+=1
            counters['max_health_latency_ms']=max(counters['max_health_latency_ms'],latency)
            status={}
            for line in Path(f'/proc/{process.pid}/status').read_text().splitlines():
                key,_,value=line.partition(':')
                if key in ('VmRSS','VmHWM','RssAnon','Threads'): status[key]=int(value.split()[0])
            samples.append({'seconds':time.monotonic()-started,**status,
                            'fds':len(list(Path(f'/proc/{process.pid}/fd').iterdir())),
                            'health_latency_ms':latency})
            await asyncio.sleep(1)
    async def workload():
        nonlocal peer
        try:
            while time.monotonic()<deadline:
                assert process.returncode is None
                captured.clear(); frames.clear()
                # Remote FRN talker -> real GSM decoder -> RF and chat link.
                offset=(root/'transmitted.pcm').stat().st_size
                for _ in range(3):
                    peer.write(b'\x02\x00\x01'+packet); await peer.drain()
                    await asyncio.sleep(.2)
                peer.write(b'\x00'); await peer.drain()
                await wait(lambda: audio_ok(captured))
                await wait(lambda: (root/'transmitted.pcm').stat().st_size>offset+6400)
                await asyncio.sleep(.6)
                # Simulated local RF -> real FRN encoder and chat input.
                captured.clear(); frames.clear()
                control(root/'squelch','O'); await asyncio.sleep(.6)
                control(root/'squelch','Z')
                try: await wait(lambda: len(frames)>=2 and audio_ok(captured))
                except AssertionError as error:
                    raise AssertionError(f'RF delivery failed: FRN frames={len(frames)}, '
                                         f'chat bytes={sum(map(len,captured))}') from error
                await asyncio.sleep(.8)
                # Deterministic helper reply -> RF and still-active FRN.
                frames.clear(); played.clear()
                offset=(root/'transmitted.pcm').stat().st_size
                reply_start=time.monotonic()
                control(root/'command','EVENT activatechat\n')
                await asyncio.wait_for(played.wait(),8)
                await wait(lambda: len(frames)>=2)
                assert (root/'transmitted.pcm').stat().st_size>offset+6400
                counters['max_reply_latency_ms']=max(counters['max_reply_latency_ms'],
                                                     (time.monotonic()-reply_start)*1000)
                await asyncio.sleep(.5)
                # Abort an incomplete GSM frame. Parser/socket recovery occurs
                # in the same SvxLink process with its real reconnect timer.
                if args.soak_active_rx:
                    peer.write(b'\x02\x00\x01'+packet); await peer.drain()
                    await asyncio.sleep(.1)
                peer.write(b'\x02\x00\x01'+packet[:113]); await peer.drain()
                drop_time=time.monotonic()
                peer.transport.abort()
                counters['frn_midframe_disconnects']+=1
                if args.soak_active_rx:
                    await wait(lambda: (root/'ptt.events').read_text().splitlines()[-1].endswith(' 0'),3)
                    counters['active_rx_disconnects']+=1
                    counters['max_ptt_release_ms']=max(counters['max_ptt_release_ms'],
                                                     (time.monotonic()-drop_time)*1000)
                peer=await reconnect()
                # Abort an incomplete helper frame, then accept its reconnect.
                previous=stats['helper_connections']; helper_ready.clear()
                current=helper_writers[-1]
                current.write(b'\x00\x00\x04\x00J{"event":"'); await current.drain()
                current.transport.abort()
                counters['helper_midframe_disconnects']+=1
                await wait(lambda: stats['helper_connections']>previous and helper_ready.is_set(),12)
                counters['cycles']+=1
                # Every tenth cycle, withhold the real FRN TX grant until its
                # production ten-second timeout forces another reconnect.
                if counters['cycles']%10==0:
                    stats['grant_tx']=False
                    control(root/'squelch','O'); await asyncio.sleep(.6)
                    control(root/'squelch','Z')
                    peer=await reconnect()
                    stats['grant_tx']=True
                    counters['grant_timeouts']+=1
                if counters['cycles']%5==0:
                    print(f"SOAK {time.monotonic()-started:.0f}s: {counters['cycles']} cycles, "
                          f"RSS={samples[-1]['VmRSS']} KiB, fds={samples[-1]['fds']}, "
                          f"max health={counters['max_health_latency_ms']:.1f} ms",flush=True)
        finally: finished.set()
    workers=[asyncio.create_task(workload()),asyncio.create_task(monitor())]
    try:
        await asyncio.gather(*workers)
        # Compare equally sized windows after warm-up, rather than treating
        # allocator/ASan quarantine initialization as a leak.
        warmup=min(120,args.soak_seconds/3)
        stable=[s for s in samples if s['seconds']>=warmup]
        assert stable,'No post-warm-up resource samples'
        window=max(1,min(60,len(stable)//3))
        first,last=stable[:window],stable[-window:]
        rss_growth=statistics.median(s['VmRSS'] for s in last)-statistics.median(s['VmRSS'] for s in first)
        fd_growth=statistics.median(s['fds'] for s in last)-statistics.median(s['fds'] for s in first)
        result.update(warmup_seconds=warmup,rss_growth_kib=rss_growth,fd_growth=fd_growth,
                      peak_rss_kib=max(s['VmRSS'] for s in samples),
                      first_window_median_rss_kib=statistics.median(s['VmRSS'] for s in first),
                      last_window_median_rss_kib=statistics.median(s['VmRSS'] for s in last))
        assert rss_growth<16384,f'RSS grew by {rss_growth} KiB after warm-up'
        assert fd_growth<=4,f'Possible descriptor leak: median growth {fd_growth}'
        assert counters['cycles']>0
        result['status']='passed'
        print(f"PASS continuous soak: {time.monotonic()-started:.1f}s, "
              f"{counters['cycles']} cycles, RSS growth {rss_growth} KiB, fd growth {fd_growth}",flush=True)
    except BaseException as error:
        result['status']='failed'; result['error']=repr(error)
        raise
    finally:
        for task in workers: task.cancel()
        await asyncio.gather(*workers,return_exceptions=True)
        result['elapsed_seconds']=time.monotonic()-started
        result['traffic']=dict(stats)
        if args.soak_report:
            args.soak_report.parent.mkdir(parents=True,exist_ok=True)
            args.soak_report.write_text(json.dumps(result,indent=2)+'\n')
