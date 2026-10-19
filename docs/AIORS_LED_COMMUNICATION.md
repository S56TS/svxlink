# AIORS LED communication

Install the updated SvxLink binary and `events.d/local/aiors.tcl` together.
Restart `svxlink.service` after installation; a reboot is not required.
The script logs a warning if the binary lacks the native `aiorsSetLed` command.

RX/TX hooks call `aiorsSetLed a|b rx|tx on|off`. This only updates one of four
desired states under a short mutex. A single worker is shared by every logic
instance, so updates to an LED cannot race through independent command queues.
The most recent event for an LED wins, including events from different logics.
The hooks do not run subprocesses, wait for replies, or access the filesystem.
TX events no longer reinstall hooks and reread configuration files.

The worker uses the existing `/run/aiors/cmd/cmd<PID>_<monotonic-ms>` protocol,
with atomic command publication and exact acknowledgement checking. It sends
one command at a time. Request/reply files and descriptors are cleaned up after
success, failure, timeout and shutdown. No LED helper processes are spawned.
The command directory must already exist and be writable by SvxLink's user;
the AIORS installer grants access through the `aiors` group.

A reply has a two-second deadline. Failures retry with delays from 250 ms up
to five seconds, while the audio event loop continues. Warnings are limited
to one per LED per 30 seconds, with a recovery message after success. Last
known desired states are refreshed every five seconds so an aiorsd restart
does not require a new RX/TX transition to restore indicators. A new SvxLink
process initially clears all four LEDs. Timings use a monotonic clock and
64-bit request timestamps. Only four states and one request are retained;
rapid events coalesce, so a brief intermediate LED pulse can be skipped.

The BSP expires orphaned replies from live clients after ten seconds, while
preserving active RSSI stream destinations. This prevents late replies from
accumulating during long uptimes. Updating the BSP is recommended along with
SvxLink for this cleanup behavior.

Manual diagnosis on the device:

```sh
sudo -u svxlink /usr/local/bin/aiorsctl set led a rx on
sudo -u svxlink /usr/local/bin/aiorsctl get led all
sudo -u svxlink /usr/local/bin/aiorsctl set led a rx off
journalctl -u svxlink --since '-10 minutes' | grep AIORS
```

`get led` reports the GPIO driver's cached output state; physical illumination
still needs checking on the device. Normal CLI updates from other tools can
temporarily override LEDs; SvxLink's periodic refresh restores its desired state.

Run `sh scripts/test-aiors-leds.sh` for worker fault and Tcl dispatch tests.
Pass the aiors_bsp checkout as the first argument for the real BSP CLI/GPIO
integration test: `sh scripts/test-aiors-leds.sh /path/to/aiors_bsp`.
