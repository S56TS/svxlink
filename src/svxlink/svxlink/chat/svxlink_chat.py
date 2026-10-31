#!/usr/bin/env python3
"""Local voice bridge for ChatLogic. SPDX-License-Identifier: GPL-2.0-or-later.

Wire format: uint32 network-order length, followed by J + UTF-8 JSON, A +
PCM16LE input, or P + uint32 epoch + PCM16LE reply. No cloud traffic before
activation. The native plugin owns pacing, PTT, source selection and DTMF.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
from dataclasses import dataclass, fields
import json
import logging
import math
import os
from pathlib import Path
import re
import struct
import time
from urllib.parse import quote

import numpy as np
from scipy.signal import resample_poly

LOG = logging.getLogger("svxlink-chat")
MAX_FRAME = 2_000_000


class RealtimeFailure(RuntimeError):
    """Diagnostic containing only selected API metadata or a startup failure."""


def error_token(value):
    # Error messages can echo arbitrary input. Log only bounded identifier fields.
    return value if isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.\[\]]{1,96}", value) else "unknown"


def api_failure(error):
    if not isinstance(error, dict):
        error = {}
    return RealtimeFailure("API error type=%s code=%s param=%s" % tuple(
        error_token(error.get(field)) for field in ("type", "code", "param")))


def failure_description(exc, startup=False):
    if isinstance(exc, RealtimeFailure):
        return str(exc)
    details = [type(exc).__name__]
    status = getattr(getattr(exc, "response", None), "status_code", None)
    if isinstance(status, int):
        details.append(f"http_status={status}")
    for field in ("rcvd", "sent"):
        frame = getattr(exc, field, None)
        code = getattr(frame, "code", None)
        if isinstance(code, int):
            details.append(f"close_{field}={int(code)}")
            # During connection startup no user audio has been uploaded. Include
            # the close reason here only; never dump an exception/response body.
            reason = getattr(frame, "reason", "")
            if startup and field == "rcvd" and isinstance(reason, str) and reason:
                key = os.environ.get("OPENAI_API_KEY", "")
                if key:
                    reason = reason.replace(key, "[redacted]")
                reason = re.sub(r"\b(?:sk|sess)-[A-Za-z0-9_-]+", "[redacted]", reason)
                details.append("reason=" + " ".join(reason.split())[:240])
    return "; ".join(details)


@dataclass
class Settings:
    port: int = 5301
    model: str = "gpt-realtime-2.1"
    voice: str = "marin"
    idle_seconds: int = 45
    max_utterance_seconds: int = 45
    max_reply_seconds: int = 30
    response_timeout_seconds: int = 45
    wake_enabled: bool = True
    wake_model: str = "hey_jarvis"
    wake_threshold: float = 0.6
    instructions: str = (
        "You are an AI voice assistant on a shared push-to-talk radio network. "
        "Your wake phrase is Hey Jarvis. Keep replies to one or two short spoken "
        "sentences, normally under 15 seconds. If only greeted, briefly invite "
        "a question. Ask for a repeat when radio audio is unclear. Do not invent "
        "callsigns or claim access to current information or radio controls. "
        "This is a shared conversation; speakers can change between turns."
    )

    @classmethod
    def load(cls, path):
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("Configuration must be a JSON object")
        unknown = data.keys() - {field.name for field in fields(cls)}
        if unknown:
            raise ValueError("Unknown settings: " + ", ".join(sorted(unknown)))
        result = cls(**data)
        for name, low, high in (
            ("port", 1, 65535), ("idle_seconds", 5, 600),
            ("max_utterance_seconds", 1, 120), ("max_reply_seconds", 1, 60),
            ("response_timeout_seconds", 5, 120),
        ):
            value = getattr(result, name)
            if type(value) is not int or not low <= value <= high:
                raise ValueError(f"{name} must be an integer in {low}..{high}")
        if type(result.wake_enabled) is not bool:
            raise ValueError("wake_enabled must be boolean")
        if not isinstance(result.wake_threshold, (float, int)) or not 0 < result.wake_threshold < 1:
            raise ValueError("wake_threshold must be between 0 and 1")
        for name in ("model", "voice", "wake_model", "instructions"):
            if not isinstance(getattr(result, name), str) or not getattr(result, name):
                raise ValueError(f"{name} must be a nonempty string")
        return result


def convert_pcm(pcm: bytes, source_rate: int, target_rate: int) -> bytes:
    if len(pcm) % 2:
        raise ValueError("Unaligned PCM16")
    if source_rate == target_rate or not pcm:
        return pcm
    divisor = math.gcd(source_rate, target_rate)
    samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32)
    converted = resample_poly(samples, target_rate // divisor, source_rate // divisor)
    return np.clip(np.rint(converted), -32768, 32767).astype("<i2").tobytes()


def tone(rate, error=False):
    # Local acknowledgement only; the plugin adds its configured TX lead-in.
    duration = 0.22 if error else 0.12
    t = np.arange(int(rate * duration)) / rate
    samples = 2500 * np.sin(2 * np.pi * (400 if error else 1000) * t)
    ramp = min(int(rate * 0.01), len(samples) // 2)
    samples[:ramp] *= np.linspace(0, 1, ramp)
    samples[-ramp:] *= np.linspace(1, 0, ramp)
    return samples.astype("<i2").tobytes()


class WakeDetector:
    def __init__(self, settings):
        from openwakeword.model import Model
        self.model = Model(wakeword_models=[settings.wake_model], inference_framework="onnx")
        self.threshold = settings.wake_threshold
        self.buffer = bytearray()

    def reset(self):
        self.buffer.clear()
        self.model.reset()

    def feed(self, pcm, rate):
        self.buffer.extend(convert_pcm(pcm, rate, 16000))
        detected = False
        while len(self.buffer) >= 2560:  # 80 ms, as recommended by openWakeWord.
            frame = bytes(self.buffer[:2560])
            del self.buffer[:2560]
            scores = self.model.predict(np.frombuffer(frame, dtype="<i2"))
            detected |= any(float(score) >= self.threshold for score in scores.values())
        return detected


class RealtimeVoice:
    """One manual-turn API session. A cancelled exchange resets its context."""
    def __init__(self, settings):
        self.settings = settings
        self.ws = None
        self.created = 0.0

    async def close(self):
        ws, self.ws = self.ws, None
        if ws is not None:
            try:
                await ws.close()
            except Exception:
                pass

    async def connect(self):
        if self.ws is not None and time.monotonic() - self.created < 1800:
            return
        await self.close()
        key = os.environ.get("OPENAI_API_KEY", "")
        if not key:
            raise RealtimeFailure("OPENAI_API_KEY is not configured")
        from websockets.asyncio.client import connect
        try:
            self.ws = await connect(
                "wss://api.openai.com/v1/realtime?model=" + quote(self.settings.model, safe=""),
                additional_headers={"Authorization": "Bearer " + key},
                open_timeout=10, close_timeout=1, max_size=MAX_FRAME,
            )
            # Read the initial event before writing. Access/quota errors can be
            # queued immediately before a server close; sending first can hide
            # that error behind a generic ConnectionClosedError.
            async with asyncio.timeout(10):
                while (await self.receive())["type"] != "session.created":
                    pass
            await self.send({
                "type": "session.update",
                "session": {
                    "type": "realtime", "output_modalities": ["audio"],
                    "instructions": self.settings.instructions,
                    "max_output_tokens": 512,
                    "audio": {
                        "input": {"format": {"type": "audio/pcm", "rate": 24000},
                                  "turn_detection": None},
                        "output": {"format": {"type": "audio/pcm", "rate": 24000},
                                   "voice": self.settings.voice},
                    },
                },
            })
            async with asyncio.timeout(10):
                while (await self.receive())["type"] != "session.updated":
                    pass
            self.created = time.monotonic()
            LOG.info("Realtime session ready")
        except BaseException as exc:
            await self.close()
            if isinstance(exc, Exception):
                raise RealtimeFailure(failure_description(exc, startup=True)) from exc
            raise

    async def send(self, event):
        await self.ws.send(json.dumps(event))

    async def receive(self):
        event = json.loads(await self.ws.recv())
        if event.get("type") == "error":
            raise api_failure(event.get("error"))
        return event

    async def reply(self, pcm, rate):
        await self.connect()
        audio = convert_pcm(pcm, rate, 24000)
        for offset in range(0, len(audio), 24000):
            await self.send({"type": "input_audio_buffer.append",
                             "audio": base64.b64encode(audio[offset:offset+24000]).decode("ascii")})
        await self.send({"type": "input_audio_buffer.commit"})
        await self.send({"type": "response.create"})
        output = bytearray()
        async with asyncio.timeout(self.settings.response_timeout_seconds):
            while True:
                event = await self.receive()
                if event.get("type") == "response.output_audio.delta":
                    output.extend(base64.b64decode(event["delta"], validate=True))
                    if len(output) > 48000 * self.settings.max_reply_seconds:
                        raise RuntimeError("Reply exceeded the transmission limit")
                elif event.get("type") == "response.done":
                    response = event.get("response", {})
                    status_details = response.get("status_details") or {}
                    if response.get("status") == "failed" and status_details.get("error"):
                        raise api_failure(status_details["error"])
                    if event.get("response", {}).get("status") != "completed" or not output:
                        raise RuntimeError("Voice response did not complete")
                    return convert_pcm(bytes(output), 24000, rate)


async def read_frame(reader):
    size = struct.unpack("!I", await reader.readexactly(4))[0]
    if size < 1 or size > MAX_FRAME:
        raise ValueError("Invalid bridge frame size")
    return await reader.readexactly(size)


class Bridge:
    def __init__(self, settings, writer, wake=None, api=None):
        self.settings, self.writer, self.wake = settings, writer, wake
        self.api = api or RealtimeVoice(settings)
        self.rate = 0
        self.epoch = 0
        self.active = False
        self.receiving = False
        self.pending_reply = False
        self.overflow = False
        self.buffer = bytearray()
        self.total_bytes = 0
        self.job = None
        self.lock = asyncio.Lock()
        self.last_activity = time.monotonic()
        self.reply_limit = settings.max_reply_seconds
        self.closed = False

    async def send_frame(self, payload):
        async with self.lock:
            self.writer.write(struct.pack("!I", len(payload)) + payload)
            await asyncio.wait_for(self.writer.drain(), 2)

    async def send_event(self, **event):
        await self.send_frame(b"J" + json.dumps(event).encode())

    async def playback(self, pcm, epoch):
        if epoch == self.epoch and not self.receiving and not self.closed:
            if len(pcm) > self.reply_limit * self.rate * 2:
                raise ValueError("Reply exceeds native playback limit")
            await self.send_frame(b"P" + struct.pack("!I", epoch) + pcm)

    async def cancel(self, reset=False):
        task, self.job = self.job, None
        interrupted = task is not None and not task.done()
        if interrupted:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        if reset or interrupted or self.pending_reply:
            await self.api.close()
        self.pending_reply = False

    async def prepare(self, epoch):
        try:
            await self.api.connect()
            await self.playback(tone(self.rate), epoch)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self.failed(epoch, exc)

    async def answer(self, pcm, epoch):
        try:
            reply = await self.api.reply(pcm, self.rate)
            if epoch != self.epoch or self.closed:
                return
            self.pending_reply = True
            await self.playback(reply, epoch)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self.failed(epoch, exc)

    async def failed(self, epoch, exc):
        LOG.warning("Voice request failed (%s); returning to standby", failure_description(exc))
        await self.api.close()
        if epoch == self.epoch:
            self.active = False
            self.pending_reply = False
            try:
                await self.playback(tone(self.rate, error=True), epoch)
            except (ConnectionError, TimeoutError):
                pass

    async def handle(self, frame):
        if frame[:1] == b"A":
            pcm = frame[1:]
            if not self.rate or len(pcm) % 2 or len(pcm) > 65536:
                raise ValueError("Invalid input audio")
            await self.send_event(event="ack", bytes=len(pcm))
            if not self.receiving or self.overflow:
                return
            self.buffer.extend(pcm)
            was_active = self.active
            if not self.active and self.wake:
                try:
                    if self.wake.feed(pcm, self.rate):
                        self.active = True
                        LOG.info("Wake phrase detected")
                except Exception as exc:
                    LOG.warning("Wake detector failed (%s); using DTMF", type(exc).__name__)
                    self.wake = None
            if self.active:
                self.total_bytes = self.total_bytes + len(pcm) if was_active else len(self.buffer)
                if self.total_bytes > self.rate * 2 * self.settings.max_utterance_seconds:
                    self.overflow = True
                    self.buffer.clear()
            if not self.active:
                # Retain two seconds before activation, including the wake phrase.
                self.buffer = self.buffer[-self.rate*4:]
            return
        if frame[:1] != b"J":
            raise ValueError("Unknown bridge frame")
        event = json.loads(frame[1:])
        kind = event["event"]
        epoch = event["epoch"]
        if type(epoch) is not int or not 0 <= epoch <= 0xffffffff:
            raise ValueError("Invalid epoch")
        if kind == "hello":
            if self.rate or event.get("version") != 1 or event.get("sample_rate") not in (8000, 16000):
                raise ValueError("Unsupported bridge configuration")
            self.rate = event["sample_rate"]
            limit = event.get("max_reply_seconds")
            if type(limit) is not int or not 1 <= limit <= 60:
                raise ValueError("Invalid playback limit")
            self.reply_limit = min(limit, self.settings.max_reply_seconds)
            self.epoch = epoch
            await self.send_event(event="ready")
            return
        if not self.rate:
            raise ValueError("Missing hello")
        if kind == "ping":
            await self.send_event(event="pong")
            return
        if kind == "begin":
            self.epoch = epoch
            await self.cancel()
            self.receiving = True
            self.buffer.clear()
            self.total_bytes = 0
            self.overflow = False
            self.last_activity = time.monotonic()
            if self.wake:
                self.wake.reset()
        elif kind == "command":
            self.epoch = epoch
            self.receiving = False
            self.buffer.clear()
            action = event.get("action")
            if action not in ("start", "stop", "reset"):
                raise ValueError("Unknown AI command")
            await self.cancel(reset=action != "start")
            if action == "stop":
                self.active = False
            elif action == "start":
                self.active = True
            self.last_activity = time.monotonic()
            if self.active:
                self.job = asyncio.create_task(self.prepare(epoch))
        elif kind == "end" and epoch == self.epoch:
            self.receiving = False
            self.last_activity = time.monotonic()
            pcm = bytes(self.buffer)
            self.buffer.clear()
            if self.overflow:
                await self.failed(epoch, ValueError("Transmission too long"))
            elif self.active and len(pcm) >= self.rate // 5 * 2:
                rms = np.sqrt(np.mean(np.frombuffer(pcm, dtype="<i2").astype(np.float32)**2))
                if rms >= 100:
                    self.job = asyncio.create_task(self.answer(pcm, epoch))
        elif kind == "interrupted":
            self.epoch = epoch
            await self.cancel(reset=True)
        elif kind == "played" and epoch == self.epoch:
            self.pending_reply = False
            self.last_activity = time.monotonic()

    async def idle_check(self):
        busy = self.receiving or self.pending_reply or (self.job and not self.job.done())
        if self.active and not busy and time.monotonic()-self.last_activity >= self.settings.idle_seconds:
            self.active = False
            await self.cancel(reset=True)
            LOG.info("Conversation timed out")

    async def close(self):
        self.closed = True
        await self.cancel(reset=True)
        self.buffer.clear()


async def serve(settings):
    if not os.environ.get("OPENAI_API_KEY"):
        LOG.warning("OPENAI_API_KEY is not configured; AI activation will fail")
    wake = None
    if settings.wake_enabled:
        try:
            wake = WakeDetector(settings)
        except Exception as exc:
            LOG.warning("Wake detector unavailable (%s); DTMF remains enabled", type(exc).__name__)
    connected = False

    async def client(reader, writer):
        nonlocal connected
        if connected:
            writer.close()
            await writer.wait_closed()
            return
        connected = True
        bridge = Bridge(settings, writer, wake)
        async def monitor():
            while True:
                await asyncio.sleep(1)
                await bridge.idle_check()
        watcher = asyncio.create_task(monitor())
        try:
            while True:
                frame = await asyncio.wait_for(read_frame(reader), 10)
                await bridge.handle(frame)
        except (asyncio.IncompleteReadError, ConnectionError, TimeoutError):
            LOG.info("SvxLink disconnected")
        except Exception as exc:
            LOG.warning("Bridge connection closed (%s)", type(exc).__name__)
        finally:
            watcher.cancel()
            await asyncio.gather(watcher, return_exceptions=True)
            try:
                await bridge.close()
                writer.close()
                await writer.wait_closed()
            except (ConnectionError, TimeoutError):
                pass
            finally:
                connected = False

    server = await asyncio.start_server(client, "127.0.0.1", settings.port, limit=MAX_FRAME)
    LOG.info("Listening on 127.0.0.1:%d; wake=%s", settings.port, bool(wake))
    async with server:
        await server.serve_forever()


async def check_api(settings):
    """Verify authentication/model/session configuration without sending audio."""
    api = RealtimeVoice(settings)
    try:
        await api.connect()
        print("Realtime connection OK; no audio sent")
        return 0
    except Exception as exc:
        LOG.error("Realtime connection check failed (%s)", failure_description(exc))
        return 1
    finally:
        await api.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="/etc/svxlink/chat.json")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check-config", action="store_true")
    action.add_argument("--download-wake-model", action="store_true")
    action.add_argument("--check-api", action="store_true", help="Check the API connection without sending audio")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    settings = Settings.load(args.config)
    if args.download_wake_model:
        import openwakeword.utils
        openwakeword.utils.download_models(model_names=[settings.wake_model])
        WakeDetector(settings)
        print("Wake model ready")
    elif args.check_config:
        print("Configuration valid; API key " + ("present" if os.environ.get("OPENAI_API_KEY") else "absent"))
    elif args.check_api:
        raise SystemExit(asyncio.run(check_api(settings)))
    else:
        try:
            asyncio.run(serve(settings))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
