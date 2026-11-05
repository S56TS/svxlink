# AIORS software and virtual validation

Validation on `integrate/upstream-aiors`, 2026-11-05. This extends the checks
in [UPSTREAM_AIORS_INTEGRATION.md](UPSTREAM_AIORS_INTEGRATION.md).

The ARM64 release executables were cross-compiled and run under QEMU Linux
user emulation in WSL. The tests use real SvxLink logic cores, codecs, timers,
audio links and sockets. A paced test sound card replaces the audio device;
`LocalSim` supplies receiver audio and controllable squelch. This is not a
complete Raspberry Pi VM and does not emulate USB electronics or RF hardware.

## Defects found and fixed

1. **Reflector audio failed in optimized builds.** `Reflector.cpp` performed
   required UDP header parsing, packet serialization and stream repositioning
   inside `assert(...)`. With `NDEBUG`, those operations disappeared. Modern
   clients authenticated, but their UDP registrations were interpreted with
   client ID zero and no speech was delivered. These operations now execute
   independently of assertions, with explicit failure handling. Both encrypted
   and legacy outgoing packet serialization were corrected. The real protocol
   3 loopback test now passes audio in both directions and after server restart.
   A legacy protocol 2 client was not separately exercised.
2. **Shutdown could crash with receiver squelch open.** Removing an active TX
   audio chain emitted a stream/idle notification and a roger event after the
   Tcl event handler was deleted. A GDB backtrace identified
   `Logic::processEvent` calling the destroyed handler. Event dispatch now
   returns when the message or event handler is absent. Clean exit and TX-off
   are verified with open squelch; the combined FRN/chat test also exits cleanly
   with FRN active and RF squelch open.

The remaining changes add repeatable tests and an optional test plugin. The
virtual sound-card plugin is enabled only by `BUILD_CHAT_TESTS`; neither it nor
`ChatTestLogic` is installed in the production payload.

## Results

| Area | Software/virtual result | What this does not prove |
| --- | --- | --- |
| Build/install | Full ARM64 `MinSizeRel` build passed; staged production installation passed; test plugins excluded. | Boot/service behavior on the installed Pi. |
| HIDRAW | Production PTT/squelch code passed normal/inverted polarity, initial report after reconnect, short/interrupted writes, missing device at startup, bounded retry, EOF/read-error recovery and closed squelch on device loss. OS device calls are fault-injected. | Actual USB disconnect/re-enumeration, device paths and electrical polarity. |
| PTT/buffering | Real `PttCtrl` timers/FIFO passed delays of 0, 100, 300 and 600 ms with blocked/partial downstream writes, exact sample preservation, flush and PTT release. | Physical PTT pin timing and radio settling time. |
| RF audio/CTCSS | Real RepeaterLogic/LocalTx chain generated the expected 1 kHz speech and 123 Hz CTCSS. A 300 ms TX delay measured approximately 311–314 ms in the test backend. `LOGIC` enabled CTCSS for linked speech; `SQL_OPEN` suppressed it for linked-only speech and enabled it for simulated RF reception. | RF deviation, modulation quality, actual tone decoding and receiver/transmitter calibration. |
| Codecs/FRN recovery | Production QsoFrn regressions passed WAV49 reference encode/decode, separate RX/TX codec state, delayed/split grants, final buffered speech, queue caps, watchdog/grant timeout reconnect, broken-pipe recovery, malformed lists and text handling. S16 passed 8,193-sample chunk/boundary tests and odd/invalid input sizes. | Lossy real networks, external FRN server interoperability and subjective speech quality. |
| Combined FRN/RF/chat | Actual ModuleFrn remained active while a loopback FRN peer exchanged GSM audio. FRN speech reached RF and ChatLogic; simulated RF reached FRN and ChatLogic; a deterministic chat reply reached RF and FRN. Received tone frequencies and nontrivial audio levels were checked. | A public FRN service, wake detection over FRN and a cloud-generated answer. |
| Reflector | Real ARM64 svxreflector and two current ReflectorLogic clients completed disposable PKI bootstrap, TLS/password authentication, encrypted UDP/Opus audio on TG 42 in both directions, and resumed audio after server restart. Received tone frequencies were checked. | Production certificates, public reflectors, firewall/NAT behavior, certificate renewal or a combined FRN/reflector/radio deployment. |
| Chat | Five native DTMF/link scenarios passed: start, unrelated command rejection, interrupted reply, malformed helper frame and stop on TG change. Virtual RF playback respected the additional 250 ms chat delay: approximately 561–564 ms including TX delay. All 23 offline Python tests passed; the additional ONNX model smoke test passed at 8 and 16 kHz. | Recognition reliability for an actual spoken wake phrase, acoustic feedback and an actual OpenAI voice response. |
| LEDs | Existing worker/Tcl fault tests passed. The current SvxLink LED worker also communicated with the actual companion BSP CLI thread compiled with `GPIO_FORCE_STUB`: all four LEDs, daemon outage/restart and state resynchronization passed. | Pin assignments, wiring, physical light output and actual GPIO driver behavior. |
| Statistics/MQTT | Four-radio parsing, fault histories, persistence, FRN formatting and the 361-metric snapshot passed. The actual ARM64 BSP MQTT agent, libmosquitto and SQLite outbox passed broker outage, reconnect, retained QoS 1 publication, PUBACK deletion, and unacknowledged redelivery after disconnect. | Production broker TLS/ACLs, actual radio measurements, server ingestion and dashboard display. |

The MQTT peer implements the MQTT 3.1.1 packets needed by this test; it is not
a full broker. The companion BSP sources are read only and compiled into
temporary test directories. Neither companion source code nor repeater
configuration is changed by these checks.

No OpenAI API key is configured in this test environment, so no paid request
was attempted. The native reply path and offline API protocol/error handling
were exercised with deterministic local peers.

The build retains the existing optional-dependency limitations: Qt is disabled;
direct RTL-SDR USB and LADSPA support are unavailable here. Existing unrelated
compiler warnings remain. The ARM64 prebuilt payload must include the updated
executables together with Async ABI 1.9 libraries/plugins; its manifest records
the exact source revision.

## Reproduce

Use Linux/WSL with the existing ARM64 cross compiler/sysroot, QEMU user
emulation, OpenSSL CLI, host GCC/G++, Tcl and the chat Python environment
(including NumPy). Configure the CMake build with `BUILD_CHAT_TESTS=ON`.

```sh
make cross-arm64 JOBS=6
BUILD=/path/to/arm64-build
SYSROOT="$PWD/build-cross/sysroot-arm64"

python3 tests/test_upstream_merge.py --build "$BUILD" --sysroot "$SYSROOT"
python3 tests/virtual_radio.py --build "$BUILD" --sysroot "$SYSROOT"
python3 tests/frn_routing_virtual.py --build "$BUILD" --sysroot "$SYSROOT"
python3 tests/reflector_virtual.py --build "$BUILD" --sysroot "$SYSROOT"
python3 tests/chat_native.py --build "$BUILD" --sysroot "$SYSROOT"
python3 -m unittest discover -s tests -p test_svxlink_chat.py -v
CHAT_TEST_WAKE_MODEL=1 python3 -m unittest discover -s tests \
  -p test_svxlink_chat.py -k real_onnx -v
python3 tests/test_svxstats_radios.py
tclsh tests/chat_control.tcl
tclsh tests/aiors_led_hooks.tcl events.d/local/aiors.tcl
```

The optional wake check requires already downloaded models. Run the LED worker
test as described in the integration report. To exercise the actual companion
CLI and MQTT agent, supply the AIORS BSP checkout and its Debian 13 ARM64
sysroot (containing libmosquitto, SQLite and cJSON):

```sh
BSP=/path/to/aiors_bsp
BSP_SYSROOT="$BSP/build/sysroot-arm64-debian13"
python3 tests/aiors_bsp_virtual.py --bsp "$BSP"
python3 tests/mqtt_virtual.py --bsp "$BSP" --sysroot "$BSP_SYSROOT"
```

Tests bind only loopback and use disposable configuration/data. The BSP bridge
fixture uses `/tmp/svxlink-aiors-bridge-test`; do not run multiple copies of
that fixture at once. None of these commands deploys to hardware.

## Remaining deployment checks

1. Repeated USB unplug/replug, missing USB devices at boot, persistent device
   names, normal/inverted PTT and squelch on each actual interface.
2. PTT timing and release on the pins; first/last syllables, audio levels, RF
   deviation and CTCSS behavior with the site's real settings.
3. Live FRN/reflector traffic and recovery, including the intended simultaneous
   links, real network interruption, DTMF controls and existing announcements.
4. Physical LEDs, actual daemon restart, measured radio diagnostics, and the
   production MQTT broker/server/dashboard path with its TLS/ACL policy.
5. A funded OpenAI API connection and spoken exchange; wake recognition with
   actual radios, voices/noise, interruption and feedback prevention.

All changes and validation are local to the integration branch. Nothing has
been pushed or installed on the repeater.
