# AIORS FRN RunCmd Diagnostics Manual

Updated 12 November 2026. Supports one or two named Simplex/Repeater logic instances.
This revision changes the FRN diagnostic command syntax. `get stats` is no longer accepted.

## Diagnostic ownership

One SvxLink process owns one shared AIORS sampler and one statistics block per configured logic.
Logic section names identify their statistics; names are not tied to a particular logic type or radio port.
Reordering `[GLOBAL] LOGICS` does not exchange persisted counters. A third logic is outside the supported diagnostics configuration and produces an explicit error.

| Shared node/hardware | Per logic |
|---|---|
| Linux boot ID and uptime; SvxLink process uptime/version/revision | RF RX/TX, squelch, events, durations and duty |
| aiorsd uptime/version/revision, hardware version, shared hardware sample age/error | FRN link transitions, users, joins/leaves, traffic and bytes |
| PSU A/B, energy, environment, ADC, fan and digital inputs | FRN RunCmd accepted/rejected/auth-failed/broadcast counters and execution times |
| Physical AI/BI/AE/BE radio health and CM108 A/B sound-card health | Logic type, callsign, RX/TX configuration names, readiness, FRN availability and counter epoch |

The MQTT agent is a separate node-wide service. Its version and uptime are added to MQTT telemetry;
FRN reports contain the Linux, SvxLink and aiorsd information available to SvxStats.

## Private message syntax and authorization

Send a private FRN text message to the node:

```text
RunCmd: <command>
```

If the receiving module has `RUN_CMD_SECRET` configured:

```text
RunCmd:<SECRET>: <command>
RunCmd:<SECRET> <command>
```

The `RunCmd:` prefix and command keywords are case-insensitive. Secrets are case-sensitive.
Named logic arguments retain their case and must match the configured section name exactly.
Explicit named queries accept letters, digits, underscores, periods and hyphens. Other section
names remain visible in the all-logic report and through their own module's default query.
Broadcast command attempts are counted under the receiving logic and ignored without a reply.
Non-RunCmd text is ignored. This allowlist does not permit arbitrary shell commands or hardware writes.

## Commands

| Command | Scope and result |
|---|---|
| `help` | Current command list and named-logic query syntax |
| `get diagnostics` | Shared node/hardware data plus the logic receiving this private FRN message |
| `get diagnostics node` | Shared process and AIORS hardware data only |
| `get diagnostics logic` | The receiving logic only |
| `get diagnostics logic RepeaterLogic2` | That exact configured logic, regardless of which module received the request |
| `get diagnostics all` | Shared data once, followed by each configured logic |
| `get version` | Live version information from `aiorsctl get version` |
| `get psu a`, `get psu b`, `get psu all` | Live physical PSU measurements from aiorsd |
| `get temp all` | Live temperature/humidity from aiorsd |
| `get radio status ai`, `bi`, `ae`, `be`, `all` | Live physical radio lifecycle state from aiorsd |

Examples:

```text
RunCmd: get diagnostics node
RunCmd: get diagnostics logic
RunCmd: get diagnostics logic LocalSimplex
RunCmd: get diagnostics all
```

The old `RunCmd: get stats` request returns `CmdReply: ERR: command not allowed`.
An unknown named logic returns an explicit error; it never selects another logic implicitly.
Hardware queries identify physical channels or ports. `RepeaterLogic2` does not mean hardware channel B.

## Replies

Replies use `CmdReply:` and are numbered when multiple FRN messages are needed.
Each private message is bounded by the 240-character FRN text limit. Long diagnostic reports
are split across messages rather than truncating the remaining fields. Reassemble numbered
parts in order; a key/value token may cross a message boundary.

Shared groups start with:

```text
DIAGNOSTICS scope=node ...
```

Logic groups identify the section name:

```text
DIAGNOSTICS scope=logic logic=RepeaterLogic type=Repeater state=ready ...
```

Logic names and metadata that contain characters unsuitable for flat key/value tokens use
percent encoding. For example, a period is represented as `%2E`. MQTT decodes these names
back to their original JSON object keys.

Configured but failed logics remain in the report with `state=failed`; their live measurements
are `na`, not falsely reported as idle/zero. When a logic has no loaded FRN module,
`frn_enabled=0` and its FRN/FRN-command fields are `na`. An unused second slot is omitted.

## Measurements and freshness

Shared `aiors_*` measurements come from the single cached hardware sample. Inspect
`aiors_sample_ts_unix_s`, `aiors_sample_age_s`, `aiors_sample_ok` and `aiors_sample_error`.
`get diagnostics node` does not issue another UART/ADC sampling request. The direct PSU,
temperature and radio commands above query aiorsd on demand.

Per-logic RF and FRN state is updated by the receiving logic's own callbacks. RF RX/TX and
squelch windows, durations, event counts, FRN lists/peaks/uniques, link ages, byte totals and
command statistics are independent. Simultaneous activity on two logics must not clear the
other logic's flags. The rolling window uses 60 one-minute buckets; windows start fresh when
SvxLink restarts. Lifetime counters are restored by logic name.

Representative families:

| Family | Meaning |
|---|---|
| `rf_tx_*`, `rf_rx_*` | RF activity, hourly events/durations/duty and persistent totals |
| `sql_*`, `sq_*` | Squelch activity, hourly events/duration and persisted open time/events |
| `frn_tx_*`, `frn_rx_*` | Per-logic FRN activity, ages, durations and bytes |
| `frn_link_*` | Per-logic link state, transition ages/events and lifetime totals |
| `frn_users*`, `frn_user_join_1h`, `frn_user_leave_1h` | This logic's observed FRN user list and window counters |
| `cmd_*` | Commands received through this logic, even if a query requests another logic |
| `counter_epoch_unix_s` | Starting baseline of that logic's persistent counters |

## Configuration

Logic names may be chosen freely; their TYPE controls behavior:

```ini
[GLOBAL]
LOGICS=LocalRepeater,LocalSimplex
STATS_PRIMARY_LOGIC=LocalRepeater
STATS_INTERVAL=600
STATS_AIORS_SAMPLE_INTERVAL=30

[LocalRepeater]
TYPE=Repeater
RX=Rx1
TX=Tx2
MODULES=ModuleFrn

[LocalSimplex]
TYPE=Simplex
RX=Rx2
TX=Tx1

[ModuleFrn]
AIORSCTL_PATH=/usr/local/bin/aiorsctl
RUN_CMD_SECRET=
```

This is a fragment: retain the normal callsign, audio, PTT/COS and module settings needed by
your installation. One Simplex plus one Repeater, two Simplex, or two Repeater instances are
supported. `STATS_PRIMARY_LOGIC` selects only the temporary legacy MQTT metric aliases;
the first LOGICS entry is the default if it is omitted. It does not change radio routing.

For two logics, set the existing node identity explicitly in `/etc/aiors-mqtt/agent.conf`:

```ini
[agent]
ENABLED=1
NODE_ID=327CRMCE0
```

The node identity remains shared; no second broker account or MQTT service is required.

## History and upgrades

The statistics snapshot is schema 4. AIORS hardware input remains diagnostics schema 5.
Persistent totals use schema 2 with `logic.<encoded-section-name>.<metric>` entries.
Shared AIORS energy/fault totals retain their existing keys and continue once per node.
Old mixed RF/FRN/command totals are preserved under `legacy.*`; they are not copied into
either new logic. `legacy_logic_totals_available=1` identifies that archive. Per-logic counters
start with their own epoch. A renamed section starts a new identity; reordering does not.

MQTT keeps its existing node topics and envelope schema 1, adding a `logics` object and
explicit event scopes. See the deployment MQTT manual for JSON types, server ingestion,
offline queues and the temporary primary-logic aliases.

## Source and validation

| Area | Source |
|---|---|
| Scoped counters, history, hardware sampler and formatting | `src/svxlink/svxlink/SvxStats.h`, `SvxStats.cpp` |
| Logic registration and readiness | `src/svxlink/svxlink/svxlink.cpp` |
| RF/squelch attribution | `src/svxlink/svxlink/Logic.cpp` |
| FRN attribution, authentication, scopes and replies | `src/svxlink/modules/frn/ModuleFrn.cpp` |
| Overlap, persistence, reordering and radio tests | `tests/test_svxstats_radios.py` |
| Actual command parser/authentication tests | `tests/test_frn_diagnostic_commands.py` |

Core statistics serialization remains on the event thread. One background writer handles
the combined snapshot, history and totals; no second hardware sampler or writer is started.
