# AIORS FRN RunCmd Diagnostics Report

Generated from the current SvxLink workspace on 2026-08-17.

## Summary

The FRN messages module implements a private text-message command bridge named `RunCmd`. Incoming FRN text messages are parsed by `QsoFrn`, emitted to `ModuleFrn`, validated against a small allowlist, and then answered with FRN private text messages prefixed with `CmdReply:`.

There are six allowlisted RunCmd commands:

| RunCmd command | Implemented behavior | Data source |
| --- | --- | --- |
| `help` | Internal reply listing the supported commands. | `ModuleFrn.cpp` allowlist |
| `get stats` | Internal SvxLink diagnostics report, split into numbered FRN text replies. | `SvxStats` counters |
| `get psu a` | Runs external `aiorsctl get psu a`. | AIORS controller via `aiorsctl` |
| `get psu b` | Runs external `aiorsctl get psu b`. | AIORS controller via `aiorsctl` |
| `get psu all` | Runs external `aiorsctl get psu all`. | AIORS controller via `aiorsctl` |
| `get temp all` | Runs external `aiorsctl get temp all`. | AIORS controller via `aiorsctl` |

The AIORS Tcl integration in `events.d/local/aiors.tcl` does not directly generate the `get stats` diagnostic reply. It handles AIORS runtime integration by wrapping SvxLink RX/TX Tcl event procedures and calling `aiorsctl set led ...`. The PSU and temperature RunCmd commands use the same external AIORS control entry point, `aiorsctl`, but the actual sensor-reading implementation is not present in this workspace.

## FRN Message Flow

1. `QsoFrn::handleCommand` sees an FRN `DT_TEXT_MESSAGE` response and switches to list receive mode.
2. `QsoFrn::handleList` parses received text messages in either triple-list form (`from_id`, `message`, `scope`) or XML form (`<ID>...</ID><MS>...</MS>`).
3. Parsed messages are emitted through `textMessageReceived(from_id, msg, scope)`.
4. `ModuleFrn::onTextMessageReceived` calls `parseAndAuthorizeCmd`.
5. Non-RunCmd messages are ignored.
6. Broadcast RunCmd attempts are ignored and recorded in stats.
7. Private RunCmd messages are either rejected with `CmdReply: ERR: ...`, handled internally, or forwarded to `aiorsctl`.
8. Replies are sent back through `QsoFrn::sendTextMessage`, which formats a private FRN `TM:<ID>...</ID><MS>...</MS>` frame.

## RunCmd Syntax and Authorization

RunCmd messages are recognized case-insensitively by the `RunCmd:` prefix.

When `[ModuleFrn] RUN_CMD_SECRET` is empty, the accepted form is:

```text
RunCmd: <command>
```

When `[ModuleFrn] RUN_CMD_SECRET` is set, the accepted forms are:

```text
RunCmd:<SECRET>: <command>
RunCmd:<SECRET> <command>
```

After optional secret handling, the command text is trimmed, converted to lower case, and checked against the allowlist in `supportedRunCmds()`.

Important guardrails:

- Only private/direct FRN messages are executed (`scope == "P"`).
- Broadcast RunCmd messages are logged, counted as broadcast attempts, and not replied to.
- Unknown commands return `CmdReply: ERR: command not allowed`.
- Empty commands return `CmdReply: ERR: empty command`.
- Auth failures return `CmdReply: ERR: auth failed`.
- Only one external `aiorsctl` command is allowed at a time; concurrent external requests receive `CmdReply: ERR: busy`.
- External command execution has a 3 second timeout.

## Implemented Commands

### `help`

`help` is handled internally by `ModuleFrn`. It returns the current command allowlist:

```text
Supported commands: get stats; get psu a; get psu b; get psu all; get temp all; help;
```

The reply is sent through `sendCmdReplyChunked`, so it is prefixed with `CmdReply:` and chunked if it ever grows beyond the FRN text message size limit.

### `get stats`

`get stats` is handled internally. It does not call `aiorsctl`.

The command calls:

```cpp
SvxStats::instance().formatStatsGroups()
```

The resulting groups are sent as numbered private replies:

```text
CmdReply: [1/N] STATS ...
CmdReply: [2/N] FRN TX ...
CmdReply: [3/N] FRN RX ...
CmdReply: [4/N] RF TX ...
CmdReply: [5/N] RF RX ...
CmdReply: [6/N] CMD ...
```

Each `get stats` reply is clamped to the FRN text limit of 240 characters.

The current stats groups are:

| Stats group | Fields included |
| --- | --- |
| `STATS` | uptime, current FRN users, peak users in the last hour, unique users in the last hour, FRN link up/down events, FRN RX/TX ages, joins/leaves |
| `FRN TX 1h` | event count, total seconds, min/max seconds, duty percentage, transmitted FRN bytes |
| `FRN RX 1h` | event count, total seconds, min/max seconds, duty percentage, received FRN bytes |
| `RF TX 1h` | event count, total seconds, min/max seconds, duty percentage, total RF TX hours |
| `RF RX 1h` | event count, total seconds, min/max seconds, duty percentage, total RF RX hours |
| `CMD 1h` | accepted commands, rejected commands, auth failures, broadcast attempts, average command execution time, max command execution time |

Note: `SvxStats` has fields for FRN link up/down events. In this workspace I found the `onFrnLinkUp()` and `onFrnLinkDown()` methods, but did not find call sites that invoke them. Unless those calls exist outside this checkout or are added later, those link counters may remain zero.

### `get psu a`

`get psu a` is forwarded as:

```text
<AIORSCTL_PATH> get psu a
```

With the default configuration this becomes:

```text
/usr/local/bin/aiorsctl get psu a
```

The reply is taken from stdout, or stderr if stdout is empty, or `OK` if both streams are empty.

### `get psu b`

`get psu b` is forwarded as:

```text
<AIORSCTL_PATH> get psu b
```

It uses the same stdout/stderr/`OK` reply behavior as `get psu a`.

### `get psu all`

`get psu all` is forwarded as:

```text
<AIORSCTL_PATH> get psu all
```

It is expected to return the AIORS power-supply diagnostics for all supported PSU channels.

### `get temp all`

`get temp all` is forwarded as:

```text
<AIORSCTL_PATH> get temp all
```

It is expected to return all temperature diagnostics exposed by the AIORS controller.

## How Diagnostics Are Pulled

### SvxLink/FRN stats diagnostics

The `get stats` command is served from `SvxStats`, a C++ singleton that aggregates runtime state.

The data path is:

| Diagnostic source | How it enters `SvxStats` |
| --- | --- |
| FRN client list | `QsoFrn` emits `frnClientListReceived`; `ModuleFrn::onFrnClientListReceived` converts the list to strings and calls `SvxStats::onFrnClientListUpdate`. |
| FRN TX/RX activity | `QsoFrn` emits `stateChange`; `ModuleFrn::onQsoStateChange` maps FRN TX audio states to TX-active and `STATE_RX_AUDIO` to RX-active. |
| FRN byte counts | `QsoFrn` emits `frnTxBytes` and `frnRxBytes`; `ModuleFrn` forwards those byte counts into `SvxStats`. |
| RF squelch/RX | `Logic::squelchOpen` calls `SvxStats::onSquelchState` and `SvxStats::onRfRxState`. |
| RF TX | `Logic::transmitterStateChange` calls `SvxStats::onRfTxState`. |
| RunCmd command metrics | `ModuleFrn::onTextMessageReceived` increments accepted, rejected, auth-failure, broadcast-attempt, and execution-time counters. |
| Periodic stats logging | `svxlink.cpp` starts `SvxStats` using `[GLOBAL] STATS_INTERVAL` and persists totals using `[GLOBAL] STATS_PERSIST_PATH`. |

### AIORS hardware diagnostics

The hardware-facing diagnostics are pulled by executing the external AIORS control program:

```text
aiorsctl get psu a
aiorsctl get psu b
aiorsctl get psu all
aiorsctl get temp all
```

`ModuleFrn` does not parse AIORS hardware readings itself. It captures stdout/stderr from `aiorsctl`, sanitizes/chunks the text reply, and sends it back to the requesting FRN client.

The `aiorsctl` implementation and the underlying AIORS daemon/sensor code were not present in this SvxLink workspace, so this report can identify the command interface and call path, but not the exact ADC/I2C/GPIO/sensor code used by AIORS to produce PSU and temperature values.

### AIORS Tcl integration

`events.d/local/aiors.tcl` integrates SvxLink logic events with AIORS hardware control:

- It resolves `aiorsctl` from `/usr/local/bin/aiorsctl`, `/usr/bin/aiorsctl`, or `auto_execok aiorsctl`.
- It maps RX/TX IDs to AIORS channel `a` or `b`.
- It wraps SvxLink `transmit` Tcl procedures and calls `aiorsctl set led <channel> tx on|off`.
- It wraps SvxLink `squelch_open` Tcl procedures and calls `aiorsctl set led <channel> rx on|off`.
- It is idempotent, so re-sourcing the file should not double-wrap the same procedures.

The C++ `Logic` class emits compatible Tcl events:

```text
squelch_open <rx_id> <0|1>
transmit <tx_id> <0|1>
```

That is how the Tcl AIORS code learns about live RX/TX state. Separately, the FRN RunCmd bridge uses `aiorsctl get ...` to query AIORS diagnostics on demand.

## Configuration

The RunCmd bridge is configured in `[ModuleFrn]`.

```ini
# Path to aiorsctl. If not set, defaults to /usr/local/bin/aiorsctl
AIORSCTL_PATH=/usr/local/bin/aiorsctl

# Optional shared secret. If set, RunCmd messages must include it.
RUN_CMD_SECRET=
```

The relevant defaults are:

- `AIORSCTL_PATH` defaults to `/usr/local/bin/aiorsctl`.
- `RUN_CMD_SECRET` defaults to empty.
- FRN text replies are limited to 240 characters per private message.
- External RunCmd execution timeout is 3 seconds.

## Source References

| Area | Source |
| --- | --- |
| RunCmd configuration load | `src/svxlink/modules/frn/ModuleFrn.cpp:216` |
| FRN text message callback hookup | `src/svxlink/modules/frn/ModuleFrn.cpp:228` |
| RunCmd allowlist | `src/svxlink/modules/frn/ModuleFrn.cpp:574` |
| RunCmd parsing and authorization | `src/svxlink/modules/frn/ModuleFrn.cpp:604` |
| Reply chunking | `src/svxlink/modules/frn/ModuleFrn.cpp:657` |
| RunCmd handling | `src/svxlink/modules/frn/ModuleFrn.cpp:716` |
| `get stats` handling | `src/svxlink/modules/frn/ModuleFrn.cpp:766` |
| External `aiorsctl` execution | `src/svxlink/modules/frn/ModuleFrn.cpp:828` |
| FRN private text send | `src/svxlink/modules/frn/QsoFrn.cpp:846` |
| FRN received text parsing | `src/svxlink/modules/frn/QsoFrn.cpp:1138` |
| `SvxStats` public API | `src/svxlink/svxlink/SvxStats.h:38` |
| Stats group formatting | `src/svxlink/svxlink/SvxStats.cpp:470` |
| Core RF squelch stats source | `src/svxlink/svxlink/Logic.cpp:1075` |
| Core RF TX stats source | `src/svxlink/svxlink/Logic.cpp:1133` |
| Stats startup/persistence | `src/svxlink/svxlink/svxlink.cpp:449` |
| AIORS Tcl `aiorsctl` resolution | `events.d/local/aiors.tcl:136` |
| AIORS RX LED command | `events.d/local/aiors.tcl:179` |
| AIORS TX LED command | `events.d/local/aiors.tcl:194` |
| AIORS TX hook | `events.d/local/aiors.tcl:424` |
| AIORS RX hook | `events.d/local/aiors.tcl:466` |

