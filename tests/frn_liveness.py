"""Real-time FRN fault scenarios, used by frn_routing_virtual.py.

Runs the full repeater and production Async TCP implementation, not a timer
mock. Servers are local/disposable and no RF hardware or accounts are used.
"""
import asyncio
import time


async def run_liveness(root, control, peer, connections, stats):
    result = {}

    async def wait(predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while not predicate():
            assert time.monotonic() < deadline, 'FRN liveness condition timed out'
            await asyncio.sleep(.05)

    # No application replies during TX, as permitted by the FRN transmit trunk.
    before_frames = stats['frn_frames']
    before_connections = stats['frn_connections']
    control(root/'squelch', 'O')
    await wait(lambda: stats['frn_frames'] >= before_frames + 5)
    tx_requests = stats['frn_tx_requests']
    start = time.monotonic()
    metadata_sent = False
    probe = 0
    while time.monotonic() - start < 65:
        assert stats['frn_connections'] == before_connections, 'Healthy long TX reconnected'
        if not metadata_sent and time.monotonic() - start >= 12:
            # A server metadata update must neither terminate nor restart TX.
            peer.write(b'\x05' + b'1\r\nroom\r\n')
            await peer.drain()
            metadata_sent = True
        probe += 1
        token = 'frn-liveness-' + str(probe)
        control(root/'command', 'EVENT health ' + token + '\n')
        await wait(lambda: (root/'health').exists() and
                   (root/'health').read_text().strip() == token, 2)
        await asyncio.sleep(1)
    assert stats['frn_connections'] == before_connections
    assert stats['frn_tx_requests'] == tx_requests, 'Metadata caused a second TX grant request'
    assert stats['frn_frames'] - before_frames >= 300, 'Long TX stopped delivering frames'
    assert stats['pings_during_tx'] == 0, 'Polling corrupted the transmit trunk'
    result['silent_peer_tx_seconds'] = time.monotonic() - start
    result['silent_peer_tx_frames'] = stats['frn_frames'] - before_frames
    print('PASS FRN liveness: 65-second TX, silent server, metadata and responsive event loop', flush=True)
    rx0 = stats['frn_rx0']
    control(root/'squelch', 'Z')
    await wait(lambda: stats['frn_rx0'] > rx0)
    await asyncio.sleep(1)

    # Stop the peer from reading. Its small kernel receive buffer eventually
    # advertises a zero window. Continuous RF audio must not hide the stall.
    before_frames = stats['frn_frames']
    control(root/'squelch', 'O')
    await wait(lambda: stats['frn_frames'] >= before_frames + 5)
    previous_port = peer.get_extra_info('sockname')[1]
    old_peer_id = peer.get_extra_info('peername')
    rejected = stats['frn_rejected_logins']
    stats['frn_login_replies'][:] = ['DUPL', 'DUPL', 'ADMIN']
    peer.transport.pause_reading()
    start = time.monotonic()
    recovered = await asyncio.wait_for(connections.get(), 85)
    result['stalled_peer_recovery_seconds'] = time.monotonic() - start
    assert stats['frn_rejected_logins'] - rejected == 2
    assert recovered.get_extra_info('sockname')[1] != previous_port, 'Backup was not tried'
    peer.transport.resume_reading()
    await wait(lambda: old_peer_id in stats['frn_reset_peers'], 5)
    peer.close()
    frames_at_login = stats['frn_frames']
    await wait(lambda: stats['frn_frames'] >= frames_at_login + 5)
    result['duplicate_rejections'] = 2
    result['backup_recovery'] = True
    result['resumed_without_rf_rekey'] = True
    result['abortive_close_verified'] = True
    print('PASS FRN liveness: blocked TCP recovers, rejects two DUPL logins, uses backup and resumes RF', flush=True)

    # Isolate transport liveness from both audio submission and incoming data.
    # Keep receiving metadata, stop audio before the queue cap, and require
    # recovery of the outstanding TCP data even though the peer keeps talking.
    recovered.transport.pause_reading()
    start = time.monotonic()
    next_connection = asyncio.create_task(connections.get())

    async def metadata():
        try:
            while not next_connection.done():
                recovered.write(b'\x05' + b'0\r\n')
                await recovered.drain()
                await asyncio.sleep(1)
        except ConnectionError:
            pass

    chatter = asyncio.create_task(metadata())
    try:
        await asyncio.sleep(18)
        assert not next_connection.done(), 'Connection failed before the transport test was established'
        # RX0 remains queued behind the stuck voice data. No more samples are
        # submitted, so a growing application queue cannot cause this recovery.
        control(root/'squelch', 'Z')
        transport_recovery = await asyncio.wait_for(next_connection, 60)
    finally:
        chatter.cancel()
        await asyncio.gather(chatter, return_exceptions=True)
        if not next_connection.done():
            next_connection.cancel()
            await asyncio.gather(next_connection, return_exceptions=True)
    recovered.transport.resume_reading()
    recovered.close()
    result['transport_recovery_with_incoming_metadata_seconds'] = time.monotonic() - start
    print('PASS FRN liveness: stalled outgoing transport recovers despite incoming metadata and stopped audio', flush=True)

    # Starting a new talkspurt late in an otherwise silent idle interval
    # must grant the new exchange a fresh deadline. A valid but slow grant
    # arrives after the old receive-only deadline would have expired.
    before_connections = stats['frn_connections']
    before_frames = stats['frn_frames']
    await asyncio.sleep(28)
    stats['frn_grant_delay'] = 4
    control(root/'squelch', 'O')
    await wait(lambda: stats['frn_frames'] >= before_frames + 5, 8)
    assert stats['frn_connections'] == before_connections, 'Old idle deadline interrupted a fresh grant'
    stats['frn_grant_delay'] = 0
    rx0 = stats['frn_rx0']
    control(root/'squelch', 'Z')
    await wait(lambda: stats['frn_rx0'] > rx0)
    result['late_idle_tx_grant_delay_seconds'] = 4
    print('PASS FRN liveness: fresh TX grant deadline after 28 seconds of silent idle', flush=True)

    # TCP now reads outgoing P polls but provides no FRN replies. Idle receive
    # liveness must still be restored following the successful reconnect.
    before_connections = stats['frn_connections']
    control(root/'squelch', 'Z')
    start = time.monotonic()
    idle_recovery = await asyncio.wait_for(connections.get(), 43)
    assert stats['frn_connections'] == before_connections + 1
    result['silent_idle_recovery_seconds'] = time.monotonic() - start
    print('PASS FRN liveness: silent idle server is still detected and reconnected', flush=True)
    stats['liveness'] = result
    return idle_recovery
