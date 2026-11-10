# AIORS FRN connection reliability

This review addresses the false disconnect reported during a long RF-to-FRN
transmission, and the associated duplicate-login/recovery paths. The starting
revision was `59f9f377` on `integrate/upstream-aiors`. Validation date:
10 November 2026.

## Cause and upstream behavior

Original upstream SvxLink refreshed the connection watchdog from outgoing audio
and reconnected on watchdog expiry only while idle. AIORS commit `118b893f`
extended the watchdog to other states. Commit `e6bc32d9` subsequently removed
the outgoing-audio reset. Together, these changes could disconnect a healthy
transmitter after 30 seconds without incoming application data. The recent
upstream merge preserved this behavior.

The [FRN protocol](https://freeradionetwork.eu/frnprotocol.htm) separates the
voice-transmit branch from the receive/poll branch. A transmitter must not
depend on continuous incoming FRN replies. The fix restores upstream's outgoing
voice activity reset while retaining connection/login recovery and adding
independent transport monitoring.

## Connection and timeout policy

| Exchange | Policy |
|---|---|
| DNS/TCP connection | 30-second connection watchdog; failed attempts use delayed retries. |
| Login | 30-second deadline after TCP connects. Partial input does not continually extend it. |
| Transmit grant | Independent 10-second deadline, including a fragmented client index. Starting TX refreshes the main deadline so an old idle timer cannot interrupt this new exchange. |
| Active outgoing voice | Completed queued voice packets refresh the main watchdog, allowing transmissions longer than 30 seconds without FRN replies. Raw PCM arrival and waiting for a grant do not count as a successful voice send. |
| Outgoing transport | A separate one-second timer checks progress independently of received commands, polls, and voice submission. Outstanding data without progress for 30 seconds causes a delayed reconnect. |
| Idle receive | Ordinary polling remains enabled; no completed incoming exchange for 30 seconds causes a delayed reconnect. |
| Partial metadata/list | A 30-second exchange deadline prevents continuing outgoing voice or repeated incoming fragments from hiding a stuck list parser. |
| Receive audio tail | Existing one-second receive-tail flushing remains. Its callback does not send a poll into a subsequent transmit exchange. |
| Reconnect | Starts after five seconds; failed attempts retain increasing backoff up to two minutes. Retries continue indefinitely and alternate the configured primary/backup endpoints. Only an accepted login resets backoff. |

On Linux, transport monitoring includes Async's pending bytes and the kernel
TCP send queue. Merely moving bytes from an application buffer to the kernel
does not count as network progress. The socket also uses `TCP_USER_TIMEOUT`
where available, and a bounded send buffer. See the
[Linux TCP interface documentation](https://man7.org/linux/man-pages/man7/tcp.7.html)
for queue and timeout semantics. Other platforms retain application-queue
monitoring, but the Linux kernel acknowledgment check is Linux-specific.

Transport acknowledgments establish TCP progress, not proof that the server
decoded or redistributed the audio. FRN voice has no per-packet application
delivery receipt that this client can use for that stronger guarantee.

Forced reconnects request an abortive TCP close so old kernel-buffered voice is
discarded instead of draining to the previous server during recovery. Normal
manual disconnects retain their graceful close behavior. The local socket test
verifies that the failed peer sees a reset.

## Login, protocol and queue handling

Successful login now requires an explicit `OK`, `ADMIN`, `OWNER` or `NETOWNER`
access level. `DUPL`, `BLOCK`, `WRONG`, missing tags and unknown access levels
do not raise a link-up event or reset backoff. The version line must be numeric.
Duplicate sessions therefore receive the normal delayed retry treatment.

Client-list and other metadata updates preserve an existing transmit/grant
state. Voice can continue during the exchange without requesting a new grant;
FRN TX statistics do not gain spurious stops/starts. Text replies remain paced
and wait until the connection is idle. RF/chat samples received during login,
disconnect or remote reception are consumed without accumulating stale audio.

| Resource | Limit / recovery |
|---|---|
| Async outgoing bytes | 32 KiB of pending payload; overflow closes the connection and schedules recovery instead of growing indefinitely. |
| Requested TCP send buffer | 16 KiB; the OS can account/allocate a different actual size. |
| Pending transmit-grant audio | Existing 50-frame maximum, approximately ten seconds. Cleared on disconnect. |
| Outgoing text queue | At most 64 messages and 64 KiB of frame text. A single frame must also fit the outgoing-byte limit. Excess messages are rejected with a log entry. |
| Protocol line | 64 KiB maximum before a newline; oversized login/list lines cause reconnect. |
| List | At most 10,000 entries and 1 MiB of entry text. Negative, overflowing, nonnumeric or excessive counts cause reconnect instead of truncating and misparsing the remainder. |
| Unknown command | Reconnect rather than continue parsing an unknown frame layout. |

These are payload limits, not exact process-RAM quotas. Container and allocator
overhead are additional. Session teardown clears partial lists, queued text,
grant audio, and client presence, and flushes active received audio. No audio
from an old connection should be replayed after login to another server.

## Validation and reproduction

Recorded outcomes, source/binary hashes and timings are in
[2026-11-10-frn-reliability.json](validation/2026-11-10-frn-reliability.json).
Detailed local traces are under `build-cross/frn-*.log`.

| Final real-time scenario | Native ASan/UBSan | ARM64/QEMU |
|---|---|---|
| Silent-server transmission | 65.96 s, 335 frames | 66.01 s, 335 frames |
| Blocked peer, two DUPL rejections and backup recovery | 52.37 s | 52.37 s |
| Blocked outgoing data with incoming metadata | 40.69 s | 40.99 s |
| Silent idle detection and reconnect | 34.99 s | 34.99 s |
| Four-second grant after 28 seconds of silent idle | passed | passed |
| Failed socket reset, resumed RF and clean shutdown | passed | passed |

Recovery measurements include queue fill, retry delays and injected login
rejections, rather than measuring only the watchdog's timeout interval.

The initial five-minute default-quarantine ASan soak completed 23 recovery
cycles but failed the RSS-growth budget (24,282 KiB growth, no descriptor growth).
That result is retained. The documented 16 MiB quarantine repeat completed
another 23 cycles with 6,238 KiB RSS growth and no descriptor growth. A separate
three-minute run with quarantine disabled measured live allocations directly:
the post-warm-up median decreased by 980 bytes, with 408 KiB RSS growth and no
descriptor growth. This supports sanitizer retention as the cause of the first
RSS observation; it is not a claim of indefinite runtime stability. The final
socket-reset implementation has its own allocation-instrumented soak entry in
the results file.

The final forced-close code completed another 14 recovery cycles in 181.55 s
with the 16 MiB quarantine: no descriptor growth, 10,876 KiB RSS growth and a
980-byte decrease in median live heap allocation. Sanitizer/leak checks found
no errors. The intentionally failing negative control detected the old TX
watchdog defect, and the full merge suites and final focused FRN suites passed
on native Linux and ARM64. Five native chat integration cases also passed.

The regression fixture includes production `QsoFrn.cpp`; only its ModuleFrn
configuration provider is substituted. Async sockets, timers, parsers and GSM
codecs are real. The routing tests run the full production repeater, FRN and
chat plugins with a simulated audio device and local disposable servers.
They require neither credentials nor real radio transmissions.

The focused regressions cover accepted/rejected login levels, mixed/split
frames, malformed/oversized lists, PCM/text/send-queue bounds, independent
transport deadlines, delayed/capped retries, receive-tail behavior, codec
isolation, and cleanup. The existing full merge suite also checks HIDRAW,
framed reconnects, callback lifetime, certificate parsing and transmitter delay.

Real-time network scenarios cover:

1. At least 65 seconds of outgoing voice with no per-packet application replies,
   plus metadata during TX and event-loop responsiveness probes.
2. A peer that stops reading, two successive duplicate-login rejections,
   configured backup selection and RF audio resuming without re-keying.
3. A blocked outgoing path while metadata keeps arriving; after audio stops,
   recovery must still occur without relying on a growing audio queue.
4. A transmit request after 28 seconds of silent idle, followed by a valid
   four-second delayed grant that crosses the previous idle deadline.
5. A silent idle peer that continues accepting polls, followed by recovery.
6. Clean shutdown with FRN active and simulated RF squelch open.

A negative control removes only the new outgoing-voice watchdog reset from a
temporary copy. The long-TX test must then fail with a TX_AUDIO watchdog
reconnect. Production files and the good runtime are not changed by this test.

Example commands for existing builds with `BUILD_CHAT_TESTS=ON`:

```sh
python3 tests/test_upstream_merge.py --build build-cross/frn-native-asan --sanitizers
python3 tests/test_upstream_merge.py --build build-cross/arm64 \
  --sysroot build-cross/sysroot-arm64

python3 tests/frn_routing_virtual.py --build build-cross/frn-native-asan \
  --frn-liveness --liveness-report build-cross/frn-liveness-native.json \
  --soak-log build-cross/frn-liveness-native.log
python3 tests/frn_routing_virtual.py --build build-cross/arm64 \
  --sysroot build-cross/sysroot-arm64 --frn-liveness \
  --liveness-report build-cross/frn-liveness-arm64.json \
  --soak-log build-cross/frn-liveness-arm64.log

# Use a new output directory for each negative-control run.
python3 tests/frn_negative_control.py --build build-cross/frn-native-asan \
  --output build-cross/frn-negative-control-new

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:quarantine_size_mb=16
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
python3 tests/frn_routing_virtual.py --build build-cross/frn-native-asan \
  --soak-seconds 300 --soak-active-rx \
  --soak-report build-cross/frn-soak-final-native.json \
  --soak-log build-cross/frn-soak-final-native.log
```

## Deployment limits

Local tests do not connect to the colleague's actual AlterFRN server or verify
physical USB, PTT/RF/CTCSS, audio levels or routing through that live service.
Verify a long RF talkspurt and real network unplug/replug on the Pi, including
backup-server policy and recovery without restarting SvxLink. Very large or
nonstandard server lists/access extensions may require revisiting the documented
protocol limits; they are explicitly rejected rather than silently accepted.

The pre-existing modified files under `prebuilt/arm64` were not overwritten by
this review. These source fixes require deploying a rebuilt FRN module; the
existing bundle must not be assumed to contain them. The Async addition is an
inline queue-length accessor and does not change its object layout or wire format.
Build/deploy against the matching merged SvxLink/Async version; a module compiled
for this tree is not a drop-in replacement for older pre-merge firmware.
