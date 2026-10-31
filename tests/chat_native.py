"""Exercise compiled ChatLogic through SvxLink's real audio links and DTMF decoder.

Usage: python3 tests/chat_native.py --build /path/to/build [--sysroot /arm64/sysroot]
The build must enable BUILD_CHAT_TESTS. All sockets bind loopback; no API calls.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import struct
import tempfile
import numpy as np


async def read_frame(reader):
    size = struct.unpack("!I", await reader.readexactly(4))[0]
    return await reader.readexactly(size)


async def send(writer, payload):
    writer.write(struct.pack("!I", len(payload)) + payload)
    await writer.drain()


def dtmf(digits, rate):
    table = {d: (row, col) for row, keys in zip((697, 770, 852, 941), ("123A", "456B", "789C", "*0#D"))
             for col, d in zip((1209, 1336, 1477, 1633), keys)}
    samples = [np.zeros(rate // 10)]
    for digit in digits:
        a, b = table[digit]
        t = np.arange(rate // 8) / rate
        samples.extend([5000 * (np.sin(2*np.pi*a*t) + np.sin(2*np.pi*b*t)), np.zeros(rate // 10)])
    return np.concatenate(samples).astype("<i2").tobytes()


async def scenario(args, digits, interrupt=False, malformed=False, change_tg=False):
    rate = args.rate
    connected = asyncio.Future()
    def accept(reader, writer):
        if not connected.done(): connected.set_result((reader, writer))
        else: writer.close()
    server = await asyncio.start_server(accept, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="chat-native-") as directory:
        root = Path(directory)
        (root / "input.pcm").write_bytes(dtmf(digits, rate))
        (root / "svxlink.conf").write_text(f"""[GLOBAL]
LOGICS=ChatLogic,TestLogic
LINKS=TestLink
LOGIC_CORE_PATH={args.build / 'lib'}
[ChatLogic]
TYPE=Chat
SERVICE_PORT={port}
TX_DELAY_MS=0
[TestLogic]
TYPE=ChatTest
INPUT_FILE={root / 'input.pcm'}
OUTPUT_FILE={root / 'output.f32'}
SECOND_MS={2400 if interrupt else 0}
STOP_ON_PLAY={int(change_tg)}
[TestLink]
CONNECT_LOGICS=ChatLogic,TestLogic
DEFAULT_ACTIVE=1
""")
        command = [str(args.build / "bin/svxlink"), "--config", str(root / "svxlink.conf")]
        env = os.environ.copy()
        libs = str(args.build / "lib")
        if args.sysroot:
            libs += f":{args.sysroot}/usr/lib/aarch64-linux-gnu:{args.sysroot}/lib/aarch64-linux-gnu"
            command = ["qemu-aarch64-static", "-L", str(args.sysroot), "-E", "LD_LIBRARY_PATH="+libs] + command
        else:
            env["LD_LIBRARY_PATH"] = libs
        process = await asyncio.create_subprocess_exec(*command, env=env, stdout=asyncio.subprocess.PIPE,
                                                       stderr=asyncio.subprocess.STDOUT)
        writer = None
        events, epochs = [], []
        success = False
        try:
            reader, writer = await asyncio.wait_for(connected, 10)
            async with asyncio.timeout(10):
                while True:
                    frame = await read_frame(reader)
                    if frame[:1] == b"A":
                        await send(writer, b"J"+json.dumps({"event": "ack", "bytes": len(frame)-1}).encode())
                        continue
                    event = json.loads(frame[1:])
                    events.append(event)
                    kind = event["event"]
                    if kind == "hello":
                        assert event["sample_rate"] == rate
                        if malformed:
                            await send(writer, b'J{"event":{}}')
                            assert await reader.read() == b""
                            assert process.returncode is None, "Invalid JSON crashed SvxLink"
                            break
                        await send(writer, b'J{"event":"ready"}')
                    elif kind == "ping":
                        await send(writer, b'J{"event":"pong"}')
                    elif kind == "command":
                        if change_tg and event["action"] == "stop":
                            assert epochs
                            break
                        assert event["action"] == "start", event
                        epochs.append(event["epoch"])
                        duration = 2 if interrupt and len(epochs) == 1 else 0.3
                        audio = (np.ones(int(rate * duration))*3000).astype("<i2").tobytes()
                        if len(epochs) == 1:
                            await send(writer, b"P"+struct.pack("!I", event["epoch"]-1)+audio)
                        await send(writer, b"P"+struct.pack("!I", event["epoch"])+audio)
                    elif kind == "played":
                        if interrupt:
                            assert len(epochs) == 2 and event["epoch"] == epochs[-1], events
                        break
                    elif kind == "end" and digits != "981#":
                        assert not epochs
                        break
            captured = np.fromfile(root / "output.f32", dtype="<f4")
            if malformed or digits != "981#":
                assert captured.size == 0
            elif change_tg:
                assert 0 < captured.size <= rate // 50, captured.size
            elif interrupt:
                assert rate * 0.3 < captured.size < rate * 2.3, captured.size
            else:
                assert captured.size == int(rate * 0.3), captured.size
                assert np.allclose(captured, 3000 / 32768), captured[:10]
            success = True
        finally:
            if writer:
                writer.close()
                await writer.wait_closed()
            server.close()
            await server.wait_closed()
            if process.returncode is None: process.terminate()
            output, _ = await process.communicate()
            if not success or process.returncode not in (0, -15):
                print(output.decode(errors="replace"))
        print(f"PASS native: digits={digits}, interrupt={interrupt}, malformed={malformed}, change_tg={change_tg}")


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--rate", type=int, default=16000, choices=(8000,16000))
    args = parser.parse_args()
    args.build = args.build.resolve()
    if args.sysroot: args.sysroot = args.sysroot.resolve()
    for digits, interrupt, malformed in [("981#", False, False), ("1981#", False, False),
                                        ("981#", True, False), ("981#", False, True)]:
        await scenario(args, digits, interrupt, malformed)
    await scenario(args, "981#", change_tg=True)


if __name__ == "__main__":
    asyncio.run(main())
