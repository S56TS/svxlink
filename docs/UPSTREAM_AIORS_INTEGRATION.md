# Upstream integration into AIORS

Integration branch: `integrate/upstream-aiors`.

The merge combines S56TS commit `d460950c110af59383c32e7e74082d1fab6b420e`
with official upstream `master` commit
`cba03b33a0b1e83c124da3ab30cff39c7d430ffa` (20 October 2026).
Upstream adds 54 commits relative to the common ancestor. This is upstream
master, whose SvxLink application version is `1.10.99.1`, rather than a checkout
of the stable 26.05.2 release tag.

## Conflict decisions

| Files | Combined behavior |
| --- | --- |
| `src/svxlink/modules/frn/QsoFrn.cpp` | Keep AIORS reconnect/watchdog logic, grant buffering, split-grant handling, separate TX/RX GSM codecs, statistics signals, and private-message delivery. Add upstream's bounded list counts, list-state reset, and exact consumed-byte accounting for mixed line endings. Keep AIORS timer and codec cleanup. |
| `src/svxlink/trx/PttHidraw.cpp` and `.h` | Use upstream's configuration reload and device cleanup structure. Keep AIORS recovery on subsequent PTT transitions and suppress repeated outage logs. Reopen before constructing the report, so the first recovered write has the correct pin and polarity. Retry interrupted writes and reject short reports. |
| `src/svxlink/trx/SquelchHidraw.cpp` and `.h` | Use upstream's owned timer/watch objects and configuration reload. Keep startup tolerance for a missing device, retry delays growing from 250 ms to a 5-second cap, outage log suppression, and closing squelch on read failure or EOF. Invalid configuration still fails initialization. |

Other upstream changes merge without text conflicts, including callsign
sanitization in reflector Tcl events, network/audio bounds checks, reflector
server changes, and module fixes. The existing AIORS event scripts, background
LED client, statistics code, ChatLogic, ChatControl, Python voice helper, and
ModuleFrn command implementation remain unchanged from the S56TS parent.

## Software verification

The following checks passed on the integrated sources:

- Full ARM64 cross-build using the existing toolchain/sysroot, libgpiod 2.2.1,
  `MinSizeRel`, 16 kHz internal audio, systemd support, and `BUILD_CHAT_TESTS=ON`.
- Staged installation of SvxLink, ModuleFrn, ChatLogic, ChatControl, the voice
  helper/service, and AIORS local Tcl scripts. The test-only ChatTestLogic
  plugin is not installed.
- 23 offline Python chat tests, plus the downloaded ONNX wake-detector smoke
  test at both 8 kHz and 16 kHz: 24 passed in total. The wake test checks model
  loading and silent input, not recognition reliability on radio speech.
- Five compiled ChatLogic scenarios under ARM64 QEMU: DTMF activation,
  unrelated command rejection, interrupted reply, malformed helper frame,
  and conversation stop on talkgroup change. These use actual logic links,
  DTMF decoding, and captured audio; no cloud API request is made.
- Existing native LED worker fault tests and full Tcl TX/RX hooks: restart,
  retry, error/incorrect replies, FIFO protection, timeouts, coalescing,
  cleanup, shutdown, and dispatch without event-loop blocking.
- ChatControl Tcl tests with active modules, exact commands, existing site
  handler passthrough, reload, custom commands, and duplicate rejection.
- Statistics radio parsing, fault histories, persistence, FRN formatting,
  and the 361-metric snapshot checks.
- New compiled HIDRAW regressions under ARM64 QEMU: normal/inverted PTT,
  first report after reconnect, interrupted/short writes, missing device at
  startup, bounded backoff, EOF/read-error recovery, and fail-closed squelch.
- New compiled FRN regressions under ARM64 QEMU: negative/invalid/oversized
  list counts, mixed line endings, both incoming private-message formats,
  login parsing/recovery, split transmit grants, buffered final audio,
  WAV49 encode/decode reference comparisons, separate codecs, capped queues,
  transmit-grant timeout, watchdog reconnect, deferred escaped text replies,
  and reconnect after the real Async socket writer encounters a broken pipe.

The FRN regression fixture replaces only ModuleFrn's configuration-provider
shell; it compiles the production QsoFrn implementation with the actual Async
libraries, sockets, timers, and GSM library. HIDRAW tests replace OS device
calls with controlled faults. Neither fixture proves physical device behavior.

The build retains the existing optional-dependency choices: Qt is disabled;
direct RTL-SDR USB support and LADSPA are unavailable in this build environment.
The successful build includes upstream compiler warnings, primarily unused
variables/parameters; those unrelated warnings were not rewritten here.

Upstream changes the Async shared-library ABI from `1.8` to `1.9`. Install the
complete rebuilt ARM64 payload, including its libraries and plugins, together.
The previous prebuilt archive belongs to the older source revision and should
not be mixed with the integrated executable. The refreshed bundle's manifest
identifies the integration source commit and its checksum.

### Reproduce the checks

From Linux/WSL, with the existing cross toolchain and dependencies installed:

```sh
make cross-arm64 JOBS=6

# Substitute your build directory; the cross-build script prints its location.
BUILD=/path/to/arm64-build
SYSROOT="$PWD/build-cross/sysroot-arm64"

python3 tests/test_upstream_merge.py --build "$BUILD" --sysroot "$SYSROOT"
python3 -m unittest discover -s tests -p test_svxlink_chat.py -v
CHAT_TEST_WAKE_MODEL=1 python3 -m unittest discover -s tests \
  -p test_svxlink_chat.py -k real_onnx -v
python3 tests/chat_native.py --build "$BUILD" --sysroot "$SYSROOT"
python3 tests/test_svxstats_radios.py
tclsh tests/chat_control.tcl
tclsh tests/aiors_led_hooks.tcl events.d/local/aiors.tcl

g++ -std=c++11 -Wall -Wextra -pthread -I src/svxlink/svxlink \
  src/svxlink/svxlink/AiorsLedClient.cpp tests/aiors_led_client.cpp \
  -o /tmp/aiors-led-client-test
/tmp/aiors-led-client-test
```

Use the chat Python environment for chat tests. The optional wake smoke test
requires the already downloaded models. Configure `BUILD_CHAT_TESTS=ON` in the
ARM64 CMake build before running `chat_native.py`.

## Validation still needed on AIORS hardware

1. Verify normal and inverted PTT on each installed USB interface, including
   the first transmission after unplugging and reconnecting it. Confirm PTT
   releases correctly and that the radio's configured TX delay is respected.
2. Verify hardware squelch polarity, missing USB hardware at startup, repeated
   unplug/replug recovery, and squelch closing while a device is absent. A
   persistent HID device path is needed if kernel enumeration numbers change.
3. Exercise RF and FRN speech in both directions: first/last syllables,
   transmit-grant delays, short transmissions, codec audio quality, server
   disconnects, network loss, and recovery on the actual FRN service.
4. Exercise RunCmd requests/replies, physical LEDs and aiorsd restart recovery,
   actual radio diagnostics, and statistics/MQTT reporting.
5. Check announcements, delay settings, roger behavior, and existing DTMF
   commands using the deployed site configuration.
6. Test chat first on RF alone, then with FRN and the intended reflector links.
   Verify start/stop/reset, wake recognition with actual noise/accent/audio,
   interruption, PTT timing, CTCSS, and replies reaching the intended links.
   A live API check needs valid API credentials and credit; automated tests
   neither used credentials nor validated a paid voice response.

The integration and ARM64 bundle are prepared locally. They have not been
pushed to GitHub or deployed to a repeater.
