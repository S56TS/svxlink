"""Offline protocol/state tests: no microphone, radio or API credentials needed."""
import asyncio
import base64
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src/svxlink/svxlink/chat"))
import svxlink_chat as chat


def pcm(seconds=0.3, rate=16000):
    return (chat.np.sin(chat.np.arange(int(rate * seconds)) * 0.1) * 4000).astype("<i2").tobytes()


class Writer:
    def __init__(self):
        self.frames = []

    def write(self, frame):
        assert struct.unpack("!I", frame[:4])[0] == len(frame) - 4
        self.frames.append(frame[4:])

    async def drain(self):
        pass

    def replies(self):
        return [f for f in self.frames if f[:1] == b"P"]


class API:
    def __init__(self):
        self.connections = 0
        self.closes = 0
        self.inputs = []
        self.block = None

    async def connect(self):
        self.connections += 1

    async def close(self):
        self.closes += 1

    async def reply(self, audio, rate):
        self.inputs.append((audio, rate))
        if self.block:
            await self.block.wait()
        return pcm()


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.writer, self.api = Writer(), API()
        self.bridge = chat.Bridge(chat.Settings(wake_enabled=False), self.writer, api=self.api)
        await self.event("hello", version=1, sample_rate=16000, max_reply_seconds=30)

    async def asyncTearDown(self):
        await self.bridge.close()

    async def event(self, kind, epoch=1, **kwargs):
        await self.bridge.handle(b"J" + json.dumps(dict(event=kind, epoch=epoch, **kwargs)).encode())

    async def settle(self):
        if self.bridge.job:
            await self.bridge.job

    async def start(self):
        await self.event("command", action="start")
        await self.settle()

    async def utterance(self, epoch=2, audio=None):
        await self.event("begin", epoch)
        await self.bridge.handle(b"A" + (pcm() if audio is None else audio))
        await self.event("end", epoch)

    async def test_passive_long_qso_never_calls_cloud_or_plays_tone(self):
        await self.event("begin")
        for _ in range(200):
            await self.bridge.handle(b"A" + pcm())
        self.assertLessEqual(len(self.bridge.buffer), 64000)
        await self.event("end")
        self.assertFalse(self.bridge.overflow)
        self.assertFalse(self.api.inputs)
        self.assertEqual(self.api.connections, 0)
        self.assertFalse(self.writer.replies())

    async def test_dtmf_start_followup_and_stop(self):
        await self.start()
        self.assertTrue(self.bridge.active)
        await self.utterance()
        await self.settle()
        self.assertTrue(self.bridge.pending_reply)
        await self.event("played", 2)
        await self.utterance(3)
        await self.settle()
        self.assertEqual(len(self.api.inputs), 2)
        await self.event("command", 4, action="stop")
        self.assertFalse(self.bridge.active)
        self.assertFalse(self.bridge.pending_reply)
        self.assertGreater(self.api.closes, 0)

    async def test_wake_keeps_phrase_and_question(self):
        class Wake:
            def reset(self): pass
            def feed(self, audio, rate): return True
        self.bridge.wake = Wake()
        question = pcm()
        await self.utterance(audio=question)
        await self.settle()
        self.assertEqual(self.api.inputs, [(question, 16000)])

    async def test_wake_failure_keeps_dtmf_available(self):
        class Wake:
            def reset(self): pass
            def feed(self, audio, rate): raise RuntimeError("inference")
        self.bridge.wake = Wake()
        await self.utterance()
        self.assertIsNone(self.bridge.wake)
        await self.start()
        self.assertTrue(self.bridge.active)

    async def test_stop_cancels_request_and_drops_late_response(self):
        await self.start()
        self.api.block = asyncio.Event()
        await self.utterance()
        await asyncio.sleep(0)
        count = len(self.writer.replies())
        await self.event("command", 3, action="stop")
        self.api.block.set()
        await self.bridge.playback(pcm(), 2)
        self.assertEqual(len(self.writer.replies()), count)

    async def test_new_speaker_cancels_unheard_context(self):
        await self.start()
        await self.utterance()
        await self.settle()
        self.assertTrue(self.bridge.pending_reply)
        await self.event("begin", 3)
        self.assertGreater(self.api.closes, 0)
        self.assertFalse(self.bridge.pending_reply)
        self.assertTrue(self.bridge.active)
        await self.event("played", 2)  # Stale flush acknowledgement.
        self.assertTrue(self.bridge.receiving)

    async def test_rejected_playback_releases_pending_response(self):
        await self.start()
        await self.utterance()
        await self.settle()
        await self.event("interrupted", 2)
        self.assertFalse(self.bridge.pending_reply)
        self.assertGreater(self.api.closes, 0)

    async def test_api_failure_logs_identifiers_without_remote_message(self):
        error = chat.api_failure({"type": "invalid_request_error", "code": "insufficient_quota",
                                  "message": "secret-key and private transcript", "param": "model"})
        with self.assertLogs(chat.LOG, level="WARNING") as logs:
            await self.bridge.failed(1, error)
        output = " ".join(logs.output)
        self.assertIn("code=insufficient_quota", output)
        self.assertNotIn("secret-key", output)
        self.assertNotIn("private transcript", output)
        self.assertFalse(self.bridge.active)

    async def test_reset_clears_context_but_preserves_active_state(self):
        await self.start()
        await self.event("command", 2, action="reset")
        await self.settle()
        self.assertTrue(self.bridge.active)
        self.assertGreater(self.api.closes, 0)

    async def test_idle_timeout_waits_for_playback(self):
        await self.start()
        await self.utterance()
        await self.settle()
        self.bridge.last_activity = time.monotonic() - 100
        await self.bridge.idle_check()
        self.assertTrue(self.bridge.active)
        await self.event("played", 2)
        self.bridge.last_activity -= 100
        await self.bridge.idle_check()
        self.assertFalse(self.bridge.active)

    async def test_silent_and_overlong_questions(self):
        await self.start()
        await self.utterance(audio=bytes(9600))
        self.assertFalse(self.api.inputs)
        self.bridge.settings.max_utterance_seconds = 1
        await self.event("begin", 3)
        for _ in range(4):
            await self.bridge.handle(b"A" + pcm())
        await self.event("end", 3)
        self.assertFalse(self.bridge.active)
        self.assertFalse(self.api.inputs)
        self.assertEqual(len(self.bridge.buffer), 0)

    async def test_native_reply_limit(self):
        self.bridge.reply_limit = 1
        with self.assertRaises(ValueError):
            await self.bridge.playback(pcm(2), 1)

    async def test_bad_input_and_missing_handshake(self):
        with self.assertRaises(ValueError):
            await self.bridge.handle(b"A\x00")
        other = chat.Bridge(chat.Settings(), Writer(), api=API())
        with self.assertRaises(ValueError):
            await other.handle(b'J{"event":"begin","epoch":1}')


class RealtimeTests(unittest.IsolatedAsyncioTestCase):
    async def test_initial_error_is_read_before_sending_session_update(self):
        ws = AsyncMock()
        ws.recv.return_value = json.dumps({"type": "error", "error": {
            "type": "invalid_request_error", "code": "model_not_found", "param": "model",
            "message": "Private credential sk-test-key must not be logged"}})
        with patch.dict("os.environ", OPENAI_API_KEY="sk-test-key"), \
             patch("websockets.asyncio.client.connect", AsyncMock(return_value=ws)):
            with self.assertRaises(chat.RealtimeFailure) as caught:
                await chat.RealtimeVoice(chat.Settings()).connect()
        self.assertIn("code=model_not_found", str(caught.exception))
        self.assertNotIn("sk-test-key", str(caught.exception))
        ws.send.assert_not_awaited()
        ws.close.assert_awaited_once()

    async def test_startup_close_reason_redacts_credentials(self):
        from websockets.exceptions import ConnectionClosedError
        from websockets.frames import Close
        key = "sk-my-private-key"
        closed = ConnectionClosedError(Close(1008, "Access denied for " + key + "\nretry later"),
                                       Close(1008, ""), True)
        ws = AsyncMock()
        ws.recv.side_effect = closed
        with patch.dict("os.environ", OPENAI_API_KEY=key), \
             patch("websockets.asyncio.client.connect", AsyncMock(return_value=ws)):
            with self.assertRaises(chat.RealtimeFailure) as caught:
                await chat.RealtimeVoice(chat.Settings()).connect()
        text = str(caught.exception)
        self.assertIn("close_rcvd=1008", text)
        self.assertIn("Access denied", text)
        self.assertNotIn(key, text)
        self.assertNotIn("\n", text)
        # After startup, close reasons may contain arbitrary remote content.
        self.assertNotIn("Access denied", chat.failure_description(closed))

    async def test_failed_response_preserves_api_code(self):
        ws = AsyncMock()
        ws.recv.return_value = json.dumps({"type": "response.done", "response": {
            "status": "failed", "status_details": {"error": {
                "type": "insufficient_quota", "code": "insufficient_quota", "message": "private"}}}})
        api = chat.RealtimeVoice(chat.Settings())
        api.ws, api.created = ws, time.monotonic()
        with self.assertRaises(chat.RealtimeFailure) as caught:
            await api.reply(pcm(), 16000)
        self.assertIn("code=insufficient_quota", str(caught.exception))
        self.assertNotIn("private", str(caught.exception))
        await api.close()

    async def test_connection_check_sends_no_audio(self):
        api = API()
        with patch.object(chat, "RealtimeVoice", return_value=api), patch("builtins.print"):
            status = await chat.check_api(chat.Settings())
        self.assertEqual(status, 0)
        self.assertEqual(api.connections, 1)
        self.assertEqual(api.closes, 1)
        self.assertFalse(api.inputs)

    async def test_connection_check_reports_failure_and_closes(self):
        api = API()
        api.connect = AsyncMock(side_effect=chat.RealtimeFailure("API error code=model_not_found"))
        with patch.object(chat, "RealtimeVoice", return_value=api), self.assertLogs(chat.LOG, level="ERROR"):
            status = await chat.check_api(chat.Settings())
        self.assertEqual(status, 1)
        self.assertEqual(api.closes, 1)

    async def test_manual_turn_wire_schema_and_audio_roundtrip(self):
        sent = []
        events = asyncio.Queue()
        response = pcm(0.1, 24000)
        for event in [
            {"type": "session.created"}, {"type": "session.updated"},
            {"type": "input_audio_buffer.committed"},
            {"type": "response.output_audio.delta", "delta": base64.b64encode(response).decode()},
            {"type": "response.done", "response": {"status": "completed"}},
        ]:
            events.put_nowait(json.dumps(event))

        class Websocket:
            async def send(self, value): sent.append(json.loads(value))
            async def recv(self): return await events.get()
            async def close(self): pass

        async def connect(url, **kwargs):
            self.assertTrue(url.startswith("wss://api.openai.com/v1/realtime?model="))
            self.assertEqual(kwargs["additional_headers"]["Authorization"], "Bearer test-key")
            return Websocket()

        with patch.dict("os.environ", OPENAI_API_KEY="test-key"), \
             patch("websockets.asyncio.client.connect", connect):
            api = chat.RealtimeVoice(chat.Settings())
            result = await api.reply(pcm(), 16000)
            await api.close()
        self.assertEqual(len(result), 3200)
        self.assertIsNone(sent[0]["session"]["audio"]["input"]["turn_detection"])
        self.assertEqual(sent[0]["session"]["output_modalities"], ["audio"])
        self.assertEqual([e["type"] for e in sent[-2:]], ["input_audio_buffer.commit", "response.create"])
        uploaded = b"".join(base64.b64decode(e["audio"]) for e in sent if "audio" in e)
        self.assertEqual(len(uploaded), 14400)

    async def test_missing_api_key_does_not_connect(self):
        with patch.dict("os.environ", {}, clear=True):
            with self.assertRaises(RuntimeError):
                await chat.RealtimeVoice(chat.Settings()).connect()

    async def test_rejects_oversize_frame_before_reading_payload(self):
        reader = asyncio.StreamReader()
        reader.feed_data(struct.pack("!I", chat.MAX_FRAME + 1))
        with self.assertRaises(ValueError):
            await chat.read_frame(reader)


class SettingsTests(unittest.TestCase):
    def test_rejects_unknown_or_invalid_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            for value in [{"api_key": "not-allowed"}, {"port": True}, {"wake_enabled": "false"},
                          {"idle_seconds": 0}, {"wake_threshold": 1}, []]:
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError): chat.Settings.load(path)

    def test_resampling_preserves_duration_at_both_native_rates(self):
        for rate in (8000, 16000):
            audio = chat.convert_pcm(pcm(1, rate), rate, 24000)
            self.assertEqual(len(audio), 48000)
            self.assertEqual(len(chat.convert_pcm(audio, 24000, rate)), 2 * rate)


@unittest.skipUnless(os.environ.get("CHAT_TEST_WAKE_MODEL"), "Optional downloaded ONNX model")
class WakeIntegrationTests(unittest.TestCase):
    def test_real_onnx_detector_accepts_both_sample_rates(self):
        wake = chat.WakeDetector(chat.Settings())
        for rate in (8000, 16000):
            wake.reset()
            for _ in range(30):
                self.assertFalse(wake.feed(bytes(rate * 2 // 10), rate))


if __name__ == "__main__":
    unittest.main()
