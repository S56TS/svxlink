# AIORS CLI bridge soak validation

Validation on `integrate/upstream-aiors`, 2026-11-06. This extends the audio and
reconnect tests in [AIORS_SOAK_SANITIZERS.md](AIORS_SOAK_SANITIZERS.md) to the
file-based AIORS/SvxLink command bridge.

## Scope and previous fixes

The companion BSP checkout is read only, at commit
`439f5f751ee0d17770b34a0f9cfbfe9bf1cd47fe`. Relevant earlier fixes are:

- BSP `4f165e5`: expire orphaned replies after ten seconds even when the owner
  process remains alive; preserve destinations of active RSSI streams.
- BSP `be26398`: run slow USB and stress commands on a separate bounded queue,
  keeping normal CLI commands responsive.
- SvxLink `d3c4f4fa`: dispatch LED updates through one native background worker,
  coalesce desired states, bound waits/retries and periodically restore outputs.

The current SvxLink application and libraries are copied into a temporary
runtime before testing. The bridge fixture compiles the actual BSP CLI and
support sources together with production `AiorsLedClient.cpp`. GPIO uses the
BSP's supported `GPIO_FORCE_STUB`. Radios are unconfigured; UART, I2C and physical
USB devices are not initialized. A private `uhubctl` script returns a simulated
USB power state after a real one-second delay.

Each long run keeps the same bridge process, LED worker and SvxLink process
alive. It repeatedly checks exact LED replies, versions, diagnostics and
unknown-command errors. It abandons real replies and temporary artifacts under
the still-running client's PID and requires their natural expiry. CLI threads
are stopped long enough to trigger the production two-second LED timeout, then
restarted. A 50,000-event burst during each outage must return within one second.
Reset LED outputs must recover through the production five-second refresh,
without another event.

The actual SvxLink `SimplexLogic`, event loop and `SvxStats` also run throughout.
Diagnostics execute the companion's real `aiorsctl` source every five seconds,
using the normal Async::Exec collection and parser. The helper is compiled for
the host in the ARM64 test because QEMU user mode cannot directly exec ARM64
children without binfmt registration. SvxLink, its libraries, the CLI server,
the LED worker and other BSP support code are ARM64 in that run. A Tcl health
probe must complete within two seconds, including CLI outages.

The application's own LED worker uses its normal `/run/aiors/cmd` path, which
does not exist in this virtual setup; its bounded retry warnings are expected.
The integrated LED fixture uses a private command directory. Separate Tcl
tests verify that real TX/RX wrappers dispatch to the native LED command.

## Results

Completed all timed runs. The ARM64 30-minute run passed. The native 30-minute
sanitizer run completed its bridge traffic, recovery and clean-shutdown checks
without sanitizer findings, but **failed the conservative RSS-growth budget**.
That observation is retained rather than reporting an unconditional pass.

| Run | Elapsed | Explicit CLI requests | CLI restarts/resyncs | Abandoned files expired |
| --- | ---: | ---: | ---: | ---: |
| ARM64/QEMU | 30 min 8 sec | 15,552 | 96 | 6,144 / 6,144 |
| Native sanitizers, 16 MiB quarantine | 30 min 11 sec | 16,204 | 97 | 6,404 / 6,404 |
| Native sanitizers, quarantine disabled | 10 min 10 sec | 5,504 | 32 | 2,176 / 2,176 |
| Native live-allocation comparison | 10 min 11 sec | 5,434 | 32 | 2,148 / 2,148 |
| ARM64 guest-heap comparison | 10 min 7 sec | 5,164 | 32 | 2,040 / 2,040 |
| Final native monitor/allocation validation | 3 min 10 sec | 1,670 | 10 | 660 / 660 |

Across these runs: **9,786 cycles, 49,528 explicit requests, 299 CLI
restart/resync cycles and 19,572 expired abandoned files**. Request counts exclude
the worker's LED commands and SvxLink's separate diagnostics requests. All 5,128
event-loop health probes completed within the two-second requirement. Every
run had zero median descriptor and thread growth after warm-up. Shutdown checks
verified both application and bridge exits as zero; ASan, UBSan and shutdown
leak detection reported no errors in the exercised paths.

The ARM64 run collected 349 successful diagnostics samples and recorded 13
expected failures during outages. Additional explicit 7.5-second-outage controls
passed on native sanitizers and ARM64: each recorded two failures, then ended
with `aiors_sample_ok=1` and `aiors_sample_error=none`. Health probes continued
throughout, confirming that diagnostics waits did not block the event loop.

The earlier defect is also detected by the negative control described below:
the old cleanup condition leaves the live client's reply behind; the current
implementation expires it. **No new production bridge defect was identified.**

### Memory observation and follow-up

The original rule compared median RSS windows after a two-minute warm-up and
required growth below 16 MiB. In the 30-minute sanitizer run, SvxLink grew
**20,848 KiB (20.36 MiB)** by that measure, so the driver returned failure.
Using a later ten-minute baseline, its remaining growth was 1,988 KiB
(1.94 MiB) over the rest of the run. The original warning remains in the
machine-readable report. That RSS assertion occurred before the final statistics
snapshot assertion; snapshot collection/recovery was separately verified by the
successful follow-up runs and explicit timeout controls.

The ten-minute comparison with quarantine disabled passed the RSS budget:
bridge growth was 378 KiB and SvxLink growth was 6,380 KiB. Direct allocation
measurements then separated live objects from resident allocator/emulator memory:

- Native bridge, ten minutes: 89,239 to 89,090 live allocated bytes, a decrease
  of 149 bytes; peak at sampled cycle boundaries was 122,358 bytes.
- ARM64 bridge, ten minutes: guest `mallinfo2().uordblks` grew from 98,432 to
  100,608 bytes, an increase of 2,176 bytes. QEMU host RSS increased much more,
  so it is not an accurate measure of the guest's live object count.
- Final native run with the corrected monitor: live allocation growth was
  1,222 bytes, with clean sanitizer shutdown and successful statistics recovery.

These measurements support allocator/sanitizer/emulator overhead as the reason
for much of the RSS growth, rather than an accumulating CLI request queue.
They do not establish a day-long memory bound for the installed device.

The first live-allocation comparison also exposed a **test-monitor race**:
the bridge exited normally between `poll()` and `/proc` sampling, leaving one
terminal/zombie sample without `VmRSS`. Its original report records failure.
The sampler now excludes exited-process samples. Reanalysis excludes only that
single terminal sample and preserves the original status. The final native run
and both repeated timeout controls passed with the corrected sampler.

Counters, binary hashes, resource-window measurements, original failures and
follow-up results are retained in
[2026-11-06-cli-soak-results.json](validation/2026-11-06-cli-soak-results.json).
Detailed local traces remain under `build-cross/cli-*.json` and
`build-cross/cli-*.log`.

## Focused regressions

Native ASan/UBSan/LSan and ARM64/QEMU checks cover both `AIORS_HW_VERSION=100`
and `110`. All twelve positive cases passed, and all four negative controls
failed the expected old-bug assertion:

- The BSP's own reply-expiration and CLI responsiveness/version tests.
- Active RSSI destinations survive cleanup, then expire after streaming stops.
- Expired and future-dated requests are discarded without execution.
- A saturated four-entry slow queue returns busy errors while diagnostics stay
  responsive; expired queued work does not prevent later commands.
- Loss and recreation of the volatile command directory recover with the same
  LED worker, and the CLI directory-health flag follows the failure/recovery.
- Simulated monotonic timestamps beyond 25, 50 and 500 days preserve request
  filenames, CLI replies and LED refresh. Only the test binary's clock calls
  are wrapped; the host clock is unchanged. This checks timestamp widths and
  recovery, rather than simulating all effects of months of actual operation.

A negative control restores only the pre-`4f165e5` cleanup condition in a
temporary CLI source copy. The BSP cleanup assertion must fail because the
live-owner reply remains. The current implementation must pass. This confirms
that the test distinguishes the earlier defect from the fixed behavior.

The existing worker fault tests also cover wrong/error acknowledgements,
missing/oversized replies, FIFO replies, slow responses, coalescing, descriptor
cleanup and prompt shutdown. Tcl tests check mapping and direct dispatch without
Tcl timer/subprocess dependencies.

## Reproduction

The companion BSP checkout and corresponding existing application build are
required. Native execution needs GCC/G++, Python 3 and Linux. ARM64 execution
also needs the cross compiler, target sysroot and `qemu-aarch64-static`.

```sh
python3 tests/aiors_cli_soak.py --bsp /path/to/aiors_bsp \
  --build /path/to/native-sanitizer-build --sanitizers \
  --seconds 1800 --report build-cross/cli-soak-sanitizer.json

python3 tests/aiors_cli_soak.py --bsp /path/to/aiors_bsp \
  --build /path/to/arm64-build --sysroot /path/to/sysroot \
  --seconds 1800 --report build-cross/cli-soak-arm64.json

python3 tests/aiors_cli_regressions.py --bsp /path/to/aiors_bsp \
  --sanitizers --report build-cross/cli-regressions-sanitizer.json

python3 tests/aiors_cli_regressions.py --bsp /path/to/aiors_bsp \
  --sysroot /path/to/sysroot --report build-cross/cli-regressions-arm64.json

sh scripts/test-aiors-leds.sh

# Explicit diagnostics-client timeout followed by successful recovery:
python3 tests/aiors_cli_soak.py --bsp /path/to/aiors_bsp \
  --build /path/to/native-sanitizer-build --sanitizers \
  --asan-quarantine-mib 0 --seconds 45 --outage-ms 7500 \
  --report build-cross/cli-statistics-timeout.json
```

`--asan-quarantine-mib 0` selects the additional comparison without ASan's
freed-memory quarantine. The normal sanitizer run keeps the quarantine enabled
and checks leak detection at clean shutdown. Reports retain process resource
samples, binary hashes, counters and detailed logs next to the requested JSON.

## Limits

These checks do not exercise the full BSP daemon's service supervisor/watchdog,
production user/group permissions, physical GPIO/LED outputs, USB drivers or
live radio sensor reads. Diagnostics intentionally report unavailable hardware
as degraded. A repeated CLI-thread restart is not a full daemon process restart.
The board still needs real operation with its installed aiorsd, SvxLink and
service permissions, alongside the physical testing already identified.

No production source or prebuilt bundle changes are required by these tests.
