# AIORS soak and sanitizer validation

Follow-up to [AIORS_VIRTUAL_VALIDATION.md](AIORS_VIRTUAL_VALIDATION.md), on
`integrate/upstream-aiors`. These are software tests, not hardware certification.

## Final results

Completed 2026-11-06. All timed runs passed their audio, recovery, responsiveness
and resource checks and exited cleanly. The native runs produced no remaining
AddressSanitizer, UndefinedBehaviorSanitizer or LeakSanitizer findings in the
exercised paths, with leak detection enabled at shutdown.

| Continuous run | Elapsed | Traffic/fault cycles | Grant timeouts recovered | RSS growth after warm-up | Median FD growth |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 release, interrupted frames | 30 min 2 sec | 134 | 13 | 0.52 MiB | 0 |
| Native sanitizers, interrupted frames | 30 min 2 sec | 134 | 13 | 10.60 MiB | 0 |
| Updated ARM64 FRN, active speech drops | 20 min 13 sec | 90 | 9 | 0.55 MiB | 0 |
| Updated native FRN, active speech drops | 20 min 13 sec | 90 | 9 | 3.46 MiB | 0 |

Across these runs, 448 cycles each exercised FRN and helper disconnect/reconnect,
44 withheld transmit grants timed out and recovered, and 5,482 event-loop
probes completed. The slowest probe took 111.8 ms; this includes the test PTY's
polling delay. Logical PTT released within 1,116.1 ms for all 180 active-speech
disconnects, before reconnect. Peak ARM64 RSS was about 36 MiB. Instrumented
memory grew during allocator/quarantine warm-up and then largely levelled off;
the measured post-warm-up growth above is retained rather than described as zero.

The reflector sanitizer run completed **60 repeated server restarts in
605.1 seconds** with two persistent clients, bidirectional Opus audio after
every restart, clean client exits and no sanitizer findings. Median descriptor
growth was zero for both clients; post-warm-up RSS growth was 6.60 and 6.64 MiB.

Machine-readable measurements, counters, runtime hashes and shutdown results
are retained in [2026-11-06-soak-results.json](validation/2026-11-06-soak-results.json).
Detailed local traces remain under `build-cross/soak-*.json` and
`build-cross/soak-*.log`. The full native sanitizer check script and ARM64
recovery regressions passed, as did the offline chat suite (23 passed; the
separate downloaded-model smoke test was not repeated in this follow-up).

## Method

Two continuous FRN/RF/chat runs keep the same SvxLink process alive throughout:
an ARM64 `MinSizeRel` release under QEMU Linux user emulation, and an x86-64
native `RelWithDebInfo` build instrumented with AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer. The target duration is 30 minutes
per run, finishing the current cycle at the deadline.

An additional pair of 20-minute runs exercises the last FRN correction: a
complete speech packet immediately precedes the interrupted packet. PTT must
release within three seconds of the disconnect, before the five-second FRN
reconnect timer completes. These use the updated ModuleFrn runtime, separately
from the original partial-frame soak snapshots.

Each cycle exchanges real GSM WAV49 through ModuleFrn, drives a simulated RF
receiver, and plays a deterministic ChatLogic reply through FRN and LocalTx.
It then aborts an incomplete FRN audio frame and an incomplete voice-helper
frame, waits for both reconnects and checks audio again. Every tenth cycle
withholds the FRN transmit grant until the production timeout forces recovery.

A separate 10-minute native sanitizer run keeps two actual ReflectorLogic
clients alive across repeated restarts of an actual svxreflector. Periodic
Opus audio must reach both clients after each restart. The server is restarted;
the clients are continuous processes.

The test harness records Linux RSS, anonymous RSS, open file descriptors and
elapsed monotonic time. The FRN/RF/chat run also probes the Tcl/event loop once
per second, requires each reply within two seconds, and checks chat playback
completion within eight seconds. Reports include hashes of the frozen runtime
copies so a later build cannot replace a library used by an ongoing soak.
PTT observations are SvxLink TX-state events with `PTT_TYPE=NONE`; physical
pin polarity and electrical timing are not measured.

Resource checks compare post-warm-up windows, allowing less than 16 MiB RSS
growth and at most four descriptors of median growth. Actual measurements are
reported above; those bounds alone are not proof of absence of a leak.
LeakSanitizer additionally checks for unreachable allocations at clean exit.
ASan's quarantine is limited to 16 MiB to keep this bounded test practical;
the instrumented process is expected to use more memory than the release.

## Findings and corrections

The additional testing found issues that the earlier short release tests did
not expose:

1. **Framed TCP reconnect state.** Disconnect cleanup cleared outgoing queues
   but retained a partial incoming frame. A new helper connection could have
   its handshake/ACK bytes consumed as part of that old frame, interrupting
   chat reception. Cleanup now resets the receive state. A production-parser
   regression exercises 1,000 interrupted transports, split fresh frames and
   multiple frames delivered together.
2. **Callback-owner deletion leaks.** Deleting a Timer, FdWatch or Exec from
   its own callback could leak libsigc++ slot allocations. A focused native
   test reproduced 1,632 leaked bytes across 100 timers and two watches; the
   diagnostics process completion path also leaked a slot. Dispatch now keeps
   a local signal reference alive until emission returns. Regression coverage
   includes self-deleting timers, read/write watches and process exit callbacks.
3. **Repeater destructor callbacks.** UBSan caught a callback bound to
   RepeaterLogic after its derived state had been destroyed and Logic was
   tearing down active audio. The repeater now disconnects its external idle
   and tone callbacks before its members are destroyed. Open-squelch shutdown
   is checked with and without an active FRN module.
4. **Deferred audio callback binding.** UBSan reported an invalid receiver type
   when inherited AudioSink/AudioSource member pointers were bound through
   AudioProcessor's multiple inheritance. Local processor wrappers now provide
   correctly typed, trackable deferred callbacks without changing the flush or
   resume behavior.
5. **IPv4 certificate SAN read.** A four-byte IPv4 address was read as an
   `unsigned long`, which is eight bytes on these 64-bit systems. ASan caught
   the out-of-bounds read during reflector certificate startup. A four-byte
   copy fixes the width and alignment issue. The regression checks loopback,
   a documentation-network address, zero and broadcast addresses.
6. **FRN receive termination on connection loss.** A targeted regression found
   that disconnect/reconnect cleanup stopped the receive timeout without
   flushing active speech or clearing the receiving flag. The audio stream
   could remain active while the connection was down. An idempotent receive
   termination helper now ends the stream, drains buffered audio and resets
   receive state on explicit disconnect, remote disconnect and forced
   reconnect. Tests check one flush per stream and PTT release during active
   reception loss.
7. **Test result handling.** The native chat fixture previously printed
   abnormal process output without failing the test. It now rejects abnormal
   exits, including sanitizer failures. Soak reports also record shutdown
   results; cleanup failures cannot be reported as successful runs.

These changes do not alter DTMF assignments, radio settings, routing policy or
the production OpenAI API configuration.

## Sanitizer scope

The native application, project libraries and plugins are instrumented with
`-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=all`. Leak detection is enabled, with no leak suppression
file. The exercised checks include HIDRAW fault injection, FRN parsing/codecs
and recovery, framed TCP, callback lifetimes, S16 boundaries/chunks, PTT
backpressure, real repeater/transmitter audio and CTCSS, chat controls and
interruption, reflector TLS/UDP/Opus, statistics parsing/persistence and LED
worker failure/recovery.

The isolated HIDRAW fixture disables only UBSan vptr instrumentation in its
generic Squelch factory translation unit: otherwise metadata retains references
to all unrelated squelch implementations that the small fixture does not link.
The full native application's Squelch implementation retains vptr checking.
Other address/undefined checks remain enabled in the isolated fixture.

System libraries are not rebuilt with instrumentation. The native environment
uses libgpiod 1.6.3; the ARM64 release build uses the intended libgpiod 2.2.1.
Neither run opens physical GPIO or HID hardware. ThreadSanitizer/race detection
and a full Raspberry Pi kernel/driver emulation are outside these tests.

## Reproduce

Use Linux/WSL with native SvxLink development dependencies, GCC, CMake,
OpenSSL CLI and the chat Python environment containing NumPy. The build stays
separate from the ARM64 release and is not installed:

```sh
export CHAT_TEST_PYTHON=/path/to/chat-venv/bin/python
sh scripts/test-sanitizers.sh /path/to/persistent/sanitizer-build

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:quarantine_size_mb=16
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
"$CHAT_TEST_PYTHON" tests/frn_routing_virtual.py \
  --build /path/to/persistent/sanitizer-build --soak-seconds 1800 \
  --soak-report /tmp/frn-soak-native.json --soak-log /tmp/frn-soak-native.log
"$CHAT_TEST_PYTHON" tests/frn_routing_virtual.py \
  --build /path/to/persistent/sanitizer-build --soak-seconds 1200 --soak-active-rx \
  --soak-report /tmp/frn-soak-active-native.json
"$CHAT_TEST_PYTHON" tests/reflector_virtual.py \
  --build /path/to/persistent/sanitizer-build --soak-seconds 600 \
  --soak-report /tmp/reflector-soak-native.json
```

For the release soak, build with `BUILD_CHAT_TESTS=ON` and use:

```sh
"$CHAT_TEST_PYTHON" tests/frn_routing_virtual.py --build /path/to/arm64-build \
  --sysroot "$PWD/build-cross/sysroot-arm64" --soak-seconds 1800 \
  --soak-report /tmp/frn-soak-arm64.json --soak-log /tmp/frn-soak-arm64.log
```

Hardware USB/GPIO/PTT/RF checks, actual wake recognition, production FRN,
reflector and MQTT services, and a funded OpenAI voice response still require
deployment testing. These bounded accelerated soaks do not replace an overnight
trial on the actual AIORS system.
