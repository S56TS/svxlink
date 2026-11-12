# AIORS + SvxLink MQTT Telemetry

Reviewed against the local AIORS, SvxLink, and deployment source on
12 November 2026. MQTT envelope schema: 1; SvxLink snapshot schema: 4;
current AIORS diagnostics schema: 5, with schema 4 compatibility.
This guide describes the source and deployment templates. Confirm the installed
binaries and active configuration when checking a running node.

## Architecture

`aiors-mqtt-agent` is a third, independent service. It is not linked into
`svxlink` or `aiorsd` and neither radio service requires it.

The agent:

1. Reads the atomic SvxLink snapshot at
   `/var/lib/svxlink/svxstats_snapshot.txt`.
2. Converts every `key=value` field into a JSON telemetry object.
3. Stores outbound telemetry, events, and command responses in
   `/var/lib/aiors-mqtt/queue.db` before publishing.
4. Publishes with MQTT QoS 1 and normally removes a delivered item after PUBACK.
   Queue-capacity eviction can also remove an item before delivery.
5. Reconnects with bounded backoff when the broker or network is unavailable.

The queue is bounded by both `MAX_QUEUE_MESSAGES` (default 10080) and
`MAX_QUEUE_BYTES` (default 50 MiB). When full, the oldest low-priority
telemetry is discarded before events or command responses. Queue priorities
are 10 for telemetry, 50 for events, and 100 for command responses. Within a
priority, older entries are discarded first. Events and responses can also
be evicted when required to meet the limits. Publishing selects the highest
priority first and then the oldest entry, with one queued publish in flight.

The limits apply to outbox row count and the SQLite sum of topic/payload text
lengths. They are not a filesystem quota for the SQLite database, WAL, indexes,
metadata, or command history. Monitor the size of `/var/lib/aiors-mqtt` as well
as `queued_messages` and `queued_bytes`. Command records older than 30 days
are pruned at startup and then hourly. The agent does not run `VACUUM` as part
of pruning.

The agent has no code path that stops or restarts `svxlink` or `aiorsd`.

The systemd unit has ordering only. It has no `Requires=` relationship with
either radio service. A broker failure, bad credential, corrupt MQTT database,
or agent crash affects MQTT reporting only.

## Node Identity

The default node ID comes from the active SvxLink logic:

1. Read `LOGICS` from `[GLOBAL]` in `/etc/svxlink/svxlink.conf`.
2. Read `CALLSIGN` from the selected logic section.
3. Convert the callsign to uppercase.

`CALLSIGN=MYCALL` is rejected. If `LOGICS` contains multiple entries, set
`NODE_ID` explicitly under `[agent]`. Characters unsafe in an MQTT topic level
are percent encoded, so `/` becomes `%2F`, `+` becomes `%2B`, and `#` becomes
`%23`. The normalized uppercase ID remains unencoded in each JSON payload as
`node_id`; the encoded value is `topic_node_id`. An explicit `NODE_ID` override
is also uppercased. Even with an override, the SvxLink configuration must be
readable and contain `[GLOBAL] LOGICS`; the `logic` field names its first entry.

## One or two independent logics

One MQTT node and one agent report a shared AIORS installation, with a named block for each
configured SvxLink logic. Names are arbitrary section names, not hardcoded `RepeaterLogic`
or `RepeaterLogic2`. Any combination of one/two Simplex and Repeater instances is supported.
The maximum is two. Logic names are case-sensitive and their original spelling is retained.

Shared `metrics` contain Linux boot/uptime, SvxLink process uptime/version/revision,
aiorsd uptime/version/revision and hardware measurements, plus MQTT agent uptime/version.
AIORS PSU/energy/environment and physical AI/BI/AE/BE/CM108 diagnostics are sampled once.
No hardware values are duplicated in the individual logic blocks.

The existing `.../<node>/telemetry` topic and envelope `schema=1` are retained. A schema-4
snapshot adds a `logics` JSON object. Example (other envelope fields and metrics omitted):

```json
{
  "schema": 1,
  "node_id": "327CRMCE0",
  "logic": "LocalRepeater",
  "primary_logic": "LocalRepeater",
  "logic_statistics_scope": "per_logic",
  "metrics": {
    "stats_schema": 4,
    "svxlink_uptime_s": 1234,
    "aiors_radio_ai_status": "ready",
    "mqtt_agent_uptime_s": 1200
  },
  "logics": {
    "LocalRepeater": {
      "type": "Repeater",
      "state": "ready",
      "callsign": "327CRMCE0",
      "rx": "Rx1",
      "tx": "Tx2",
      "frn_enabled": true,
      "frn_users": 10,
      "rf_tx_evt_1h": 12
    },
    "LocalSimplex": {
      "type": "Simplex",
      "state": "ready",
      "callsign": "327CRMCE0-2",
      "rx": "Rx2",
      "tx": "Tx1",
      "frn_enabled": false,
      "frn_users": null,
      "rf_tx_evt_1h": 7
    }
  }
}
```

One configured logic produces one object, without an empty second slot. A configured failed
logic stays present with `state=failed` and null live/statistical values. `counter_epoch_unix_s`
remains available as its historical baseline. If an FRN module is not loaded, `frn_enabled`
is false and `frn_*`/`cmd_*` values in that logic block are null, not faults or zero activity.

### Identity, routing and compatibility

Keep the existing `[agent] NODE_ID` (for example `327CRMCE0`) when adding another logic.
Two LOGICS entries require this explicit override. A single logic may still derive the node
ID from its callsign. No second broker username or service is needed.

Optional `[GLOBAL] STATS_PRIMARY_LOGIC=LocalRepeater` pins the temporary flat `frn_*`,
`rf_*`, squelch and `cmd_*` compatibility aliases under `metrics`. The first configured logic
is the default. These aliases represent only that primary logic, never combined activity.
New consumers should read logic statistics exclusively from `logics[section_name]`, so they
do not count both the scoped value and its compatibility alias. The top-level `logic` field
is retained as the primary-logic label. Shared hardware stays in `metrics`.

`rx`/`tx` name the SvxLink RX/TX configuration sections. They do not imply physical channel
A/B or internal/external radio locations: audio/COS/PTT routing remains common hardware state.
The order of LOGICS does not identify or own historical counters. Reordering the list keeps
each named logic's totals; renaming a section creates a new counter identity.

Older snapshots without logic blocks are still published with
`logic_statistics_scope=legacy_unseparated` and no `logics` object. Do not label their mixed
statistics as either new logic. Queued older messages remain deliverable in their original
format; consumers must accept both shapes during an upgrade.

### Scoped events and queue history

Hardware change events carry `scope=node`. Logic events carry `scope=logic` and the actual
`logic` name. Link up/down totals, logic readiness and FRN availability have independent
change baselines keyed by name. Reordering snapshot slots does not generate false changes.
Legacy unseparated FRN events use `scope=legacy`. Primary aliases do not generate duplicate
FRN events when scoped blocks are present. A first observation establishes a baseline.

The existing single SQLite queue, node-wide sequence and broker status remain shared.
Retained telemetry describes the full currently configured set; removing a logic does not
leave a stale retained per-logic topic. Publication cadence and hardware sampling intervals
remain the existing GLOBAL settings.

### Persistence and server changes

Shared hardware totals, including Wh/Ah and physical radio faults, continue under their
existing keys. Persistent logic totals use schema 2 and keys of the form
`logic.<percent-encoded-section-name>.<metric>`. Old mixed totals are retained under `legacy.*`
and flagged by `legacy_logic_totals_available`. They cannot be divided retrospectively and
are not copied into either logic. Per-logic epochs identify the new baseline. Rolling
one-hour windows start fresh on process restart; lifetime counters are restored by name.

Store scoped statistics by node ID plus logic name and counter epoch. Dashboards should
discover available logic names rather than assume two fixed names. Keep process/hardware
cards node-wide and display separate logic panels. A failed logic and an unconfigured logic
must be displayed differently. Avoid summing simultaneous logic duty cycles as node-wide
wall-clock duty.

The broker ACL example needs no new topic paths for this extension. The receiving server
does need ingestion/display support for `logics` and scoped events; test strict JSON
consumers before deployment. MQTT subscriptions select topics, not individual JSON fields.
The agent publishes complete telemetry; consumers select the fields they store/display.

Install the matching SvxLink and MQTT agent builds together; the older 512-field agent cannot consume the expanded two-logic snapshot.

The flat snapshot parser is bounded at 1,024 metrics and a 1 MiB input file. The regression
two-logic snapshot currently has 551 fields including primary aliases. AIORS input remains
diagnostics schema 5; unavailable physical-radio data remains node-wide null/na as before.

### Installer validation

The installer reads configuration using the new agent's `--check-config` parser under the
`aiors-mqtt` service account. Boolean forms such as true/yes/on are accepted consistently.
Configuration is validated before stopping services for binary replacement. A validation
failure is an error, never implicitly ENABLED=0. Only an explicitly disabled configuration
disables the unit. A later failed upgrade attempts to restart a previously running agent.

### Per-logic field reference

These fields occur inside each `logics[section_name]` object. Shared AIORS fields do not.
Numbers retain the existing typing rules below; metadata strings remain strings, and
`frn_enabled` is a JSON boolean. Failed/inapplicable values are null as described above.

| Field | Meaning |
|---|---|
| `type` | Configured logic TYPE, for example Simplex or Repeater. |
| `callsign` | The configured callsign for this logic. |
| `rx` | RX configuration section name. |
| `tx` | TX configuration section name. |
| `state` | configured, ready or failed. |
| `frn_enabled` | Whether an FRN module is loaded for this logic. |
| `frn_link_up_evt_1h` | Number of FRN link-up events in the last hour. |
| `frn_link_down_evt_1h` | Number of FRN link-down events in the last hour. |
| `frn_link_up_total` | Cumulative count of observed FRN link-up events. |
| `frn_link_down_total` | Cumulative count of observed FRN link-down events. |
| `frn_link_up_age_s` | Seconds since the most recent link-up event; -1 if none observed in this process. |
| `frn_link_down_age_s` | Seconds since the most recent link-down event; -1 if none observed in this process. |
| `frn_rx_age_s` | Seconds since recorded FRN receive activity, including client-list updates as well as voice data. Not exclusively time since speech; -1 if none. |
| `frn_tx_age_s` | Seconds since recorded outgoing FRN voice activity; -1 if none. |
| `frn_users` | Current count of distinct entries in the FRN client list; not necessarily distinct people. |
| `frn_users_peak_1h` | Tracked peak client count. The implementation resets it roughly hourly; it is not an exact sliding-hour maximum. |
| `frn_users_unique_1h` | Count of distinct FRN client-list identities seen during the last hour. |
| `frn_link_up_1h` | Compatibility alias of frn_link_up_evt_1h; same count, do not add them together. |
| `frn_link_down_1h` | Compatibility alias of frn_link_down_evt_1h; same count, do not add them together. |
| `frn_user_join_1h` | Count of entries added between observed FRN client lists in the last hour. |
| `frn_user_leave_1h` | Count of entries removed between observed FRN client lists in the last hour. |
| `frn_tx_evt_1h` | frn tx evt 1h; scoped to this logic. |
| `frn_tx_s_1h` | frn tx s 1h; scoped to this logic. |
| `frn_tx_min_s_1h` | frn tx min s 1h; scoped to this logic. |
| `frn_tx_max_s_1h` | frn tx max s 1h; scoped to this logic. |
| `frn_tx_bytes_1h` | Outgoing GSM voice payload bytes in the last hour; excludes FRN control commands and TCP/IP overhead. |
| `frn_tx_duty_1h` | Outgoing FRN voice duration / 3600 × 100, clipped to 0–100%; 10 means 10%. |
| `frn_rx_evt_1h` | frn rx evt 1h; scoped to this logic. |
| `frn_rx_s_1h` | frn rx s 1h; scoped to this logic. |
| `frn_rx_min_s_1h` | frn rx min s 1h; scoped to this logic. |
| `frn_rx_max_s_1h` | frn rx max s 1h; scoped to this logic. |
| `frn_rx_bytes_1h` | Received voice packet bytes including its FRN client index; excludes general client lists and TCP/IP overhead. Not exactly the same accounting as TX. |
| `rf_tx_evt_1h` | rf tx evt 1h; scoped to this logic. |
| `rf_tx_s_1h` | rf tx s 1h; scoped to this logic. |
| `rf_tx_min_s_1h` | rf tx min s 1h; scoped to this logic. |
| `rf_tx_max_s_1h` | rf tx max s 1h; scoped to this logic. |
| `rf_tx_hours_total` | rf tx hours total; scoped to this logic. |
| `rf_tx_duty_1h` | rf tx duty 1h; scoped to this logic. |
| `rf_rx_evt_1h` | rf rx evt 1h; scoped to this logic. |
| `rf_rx_s_1h` | rf rx s 1h; scoped to this logic. |
| `rf_rx_min_s_1h` | rf rx min s 1h; scoped to this logic. |
| `rf_rx_max_s_1h` | rf rx max s 1h; scoped to this logic. |
| `rf_rx_hours_total` | rf rx hours total; scoped to this logic. |
| `rf_rx_duty_1h` | rf rx duty 1h; scoped to this logic. |
| `cmd_ok_1h` | Accepted FRN RunCmd requests in the last hour; count. Acceptance does not guarantee the external command exited successfully. |
| `cmd_rej_1h` | Rejected RunCmd requests in the last hour; count. |
| `cmd_authfail_1h` | RunCmd authentication failures in the last hour; count. |
| `cmd_bc_attempt_1h` | Broadcast/non-private RunCmd attempts that were ignored; count. |
| `cmd_avg_ms_1h` | Mean measured external-command elapsed time in milliseconds; integer average, 0 if no timings. |
| `cmd_max_ms_1h` | Maximum measured external-command elapsed time in milliseconds; 0 if no timings. |
| `sql_evt_1h` | sql evt 1h; scoped to this logic. |
| `sql_s_1h` | sql s 1h; scoped to this logic. |
| `rf_tx_active` | Current RF transmit activity. |
| `rf_rx_active` | Current RF receive activity. |
| `sql_active` | Current open-squelch activity. |
| `frn_tx_active` | Current FRN transmit activity. |
| `frn_rx_active` | Current FRN receive activity. |
| `frn_link_up` | Current observed FRN connection state. |
| `counter_epoch_unix_s` | Unix-second baseline for these persistent logic counters. |
| `rf_tx_s_total` | rf tx s total; scoped to this logic. |
| `rf_tx_evt_total` | rf tx evt total; scoped to this logic. |
| `rf_rx_s_total` | rf rx s total; scoped to this logic. |
| `rf_rx_evt_total` | rf rx evt total; scoped to this logic. |
| `sq_open_s_total` | sq open s total; scoped to this logic. |
| `sq_evt_total` | sq evt total; scoped to this logic. |
| `frn_tx_s_total` | frn tx s total; scoped to this logic. |
| `frn_tx_evt_total` | frn tx evt total; scoped to this logic. |
| `frn_rx_s_total` | frn rx s total; scoped to this logic. |
| `frn_rx_evt_total` | frn rx evt total; scoped to this logic. |
| `cmd_ok_total` | cmd ok total; scoped to this logic. |
| `cmd_rej_total` | cmd rej total; scoped to this logic. |
| `cmd_authfail_total` | cmd authfail total; scoped to this logic. |
| `cmd_bc_attempt_total` | cmd bc attempt total; scoped to this logic. |
| `cmd_sum_ms_total` | cmd sum ms total; scoped to this logic. |
| `cmd_max_ms_total` | cmd max ms total; scoped to this logic. |
| `frn_tx_bytes_total` | frn tx bytes total; scoped to this logic. |
| `frn_rx_bytes_total` | frn rx bytes total; scoped to this logic. |
| `cmd_count_ms_total` | cmd count ms total; scoped to this logic. |

The FRN text interface now uses `get diagnostics`, `get diagnostics node`,
`get diagnostics logic [name]`, and `get diagnostics all`. See the updated FRN RunCmd manual.
The old FRN `get stats` syntax is deliberately not backward compatible.

## Topics

For `CALLSIGN=S5RPTAIORS`, the agent uses:

| Topic | QoS | Retained | Contents |
|---|---:|---:|---|
| `frn/aiors/v1/S5RPTAIORS/status` | 1 | yes | Online/offline state, agent version, boot ID, command state, queue depth |
| `frn/aiors/v1/S5RPTAIORS/telemetry` | 1 | yes | Complete latest SvxLink statistics snapshot, including AIORS diagnostics |
| `frn/aiors/v1/S5RPTAIORS/events` | 1 | no | Important AIORS state and fault transitions |
| `frn/aiors/v1/S5RPTAIORS/command/request` | 1 | no | Server-to-node structured control request; subscription is disabled by default |
| `frn/aiors/v1/S5RPTAIORS/command/response` | 1 | no | Durable result for a command request |

The broker Last Will sets retained status to `offline` after an unexpected
disconnect. A graceful service stop publishes `offline` with reason
`service_stopped`.
The Last Will payload is prepared when the agent initializes its MQTT client;
its `published_at` can therefore predate the disconnect. Record the server's
receipt time when dating an offline transition.

Status messages also report `agent_version`, `agent_revision`,
`commands_enabled`, `queued_messages`, `queued_bytes`, `queue_message_limit`,
and `queue_byte_limit`. Queue depths are measured when the status message is
created; they are not a continuously refreshed queue monitor.

## Telemetry Payload

Example envelope with a selected subset of metrics. Timestamps and software
identifiers are illustrative:

```json
{
  "schema": 1,
  "kind": "telemetry",
  "node_id": "S5RPTAIORS",
  "topic_node_id": "S5RPTAIORS",
  "boot_id": "42f81f04-66e6-4692-8dde-81a8d8f7e19b",
  "sequence": 1842,
  "published_at": "2026-11-03T12:30:00Z",
  "logic": "RepeaterLogic",
  "source": "/var/lib/svxlink/svxstats_snapshot.txt",
  "metrics": {
    "stats_schema": 4,
    "aiors_diagnostics_version": "5",
    "aiors_software_version": "4f165e5-dirty",
    "aiors_software_revision": "4f165e5-dirty",
    "aiors_hardware_version": "1.1",
    "ts_unix_s": 1793709000,
    "aiors_overall_status": "ok",
    "aiors_sample_ok": 1,
    "aiors_radio_ai_status": "ready",
    "aiors_radio_bi_status": "ready",
    "aiors_radio_ae_status": "disabled",
    "aiors_radio_be_status": "unimplemented",
    "aiors_sa818_a_status": "ok",
    "aiors_sa818_b_status": "ok",
    "aiors_radio_ae_fault_evt_total": 0,
    "aiors_psu_a_input_voltage_v": 13.72,
    "aiors_psu_a_energy_wh_total": 126.31
  }
}
```

`na` is transmitted as JSON `null`. For other values, keys containing
`status`, `state`, `version`, `revision`, `scope`, `error`, `reason`, or
`boot_id` remain strings, even when the value looks numeric. Other fully
numeric values become JSON numbers; other nonnumeric values remain strings.
For example, `aiors_diagnostics_version` is a string, and the numeric-looking
counter `aiors_psu_a_state_change_evt_total` is also a string because its key
contains `state`. Consumers must follow the actual JSON type.

Every field in the snapshot is sent. The current snapshot groups are:

| Group | Data |
|---|---|
| Identity and uptime | Snapshot schema/time, Linux boot ID and uptime, SvxLink runtime/version/revision, AIORS runtime/version/revision/hardware version |
| FRN link | Link up/down ages and 1-hour/lifetime counters, users, joins/leaves, RX/TX age, duration, bytes, events, and duty |
| RF | RX/TX event counts, durations, min/max duration, duty, and lifetime hours |
| RunCmd | Accepted, rejected, authentication failure, broadcast attempt, average latency, and maximum latency values |
| AIORS health | Sample status/age/error, overall status, ADC/environment/digital-input status, PSU GPIO status, fan state |
| PSU A and B | Current state/fault, input voltage/current/calculated power, 1-hour and 24-hour min/max/average/sample count |
| Power totals | Per-channel Wh and Ah, measurement coverage, on/off time, fault time/events, state changes, and restart/degraded/fault totals |
| Board measurements | 3.3 V and 5 V buses, temperature, humidity, RF A/B average voltage, AI0-AI3; each includes current and 1-hour/24-hour min/max/average/sample count |
| Radio peripherals | Physical AI/BI/AE/BE lifecycle states, legacy internal A/B and CM108 A/B status, plus separate 1-hour, 24-hour, and lifetime fault event/sample/duration counters |

The authoritative field list is the `metrics` object. Consumers should ignore
unknown fields so a future `stats_schema` can add diagnostics without breaking
ingestion.

### Four physical radio ports

Radio management and reporting are independent of audio/PTT/COS routing.
The four telemetry state fields identify these physical ports:

| Metric | Physical radio |
|---|---|
| `aiors_radio_ai_status` | Internal A |
| `aiors_radio_bi_status` | Internal B |
| `aiors_radio_ae_status` | External A |
| `aiors_radio_be_status` | External B |

| State | Meaning | Accumulates radio faults |
|---|---|---|
| `ready` | Identified and configured by the radio driver | no |
| `disabled` | Radio management is disabled for this port | no |
| `unimplemented` | A configured external port has no implemented driver | no |
| `absent` | The expected module is not responding | yes |
| `mismatch` | Detected identity differs from the configured model | yes |
| `config_error` | Configuration or verification failed | yes |
| `uninitialized` | The radio has not reached an initialized lifecycle state | yes |

Per-port numeric counters use these patterns, with `<port>` equal to `ai`,
`bi`, `ae`, or `be`, and `<window>` equal to `1h`, `24h`, or `total`:

```text
aiors_radio_<port>_fault_evt_<window>
aiors_radio_<port>_fault_samples_<window>
aiors_radio_<port>_fault_s_<window>
```

The counters measure observed fault transitions, fault samples, and fault
duration. Disabled radios and unimplemented external driver placeholders
accumulate no faults. A first fault observation can add a fault sample without
adding a transition event when no earlier sample exists.

The legacy `aiors_sa818_a_status` and `aiors_sa818_b_status` fields and their
counters remain available. With diagnostics schema 5, they refer to physical
AI and BI. Their telemetry status is JSON `null` while disabled or
unimplemented; the original binary `get diagnostics all` compatibility fields
can still say `fault`. Use the explicit physical fields for new consumers.

Schema 4 is still accepted. Its legacy A/B fields retain their older meaning,
and all four explicit physical status fields are JSON `null` because physical
identity is unavailable in that schema. Existing historical counters are
preserved. The new physical-port lifetime counters start separately and are
persisted without importing old routing-dependent A/B totals.

### Sampling, publication, and freshness

The AIORS deployment template uses these settings:

| Setting | Current template value | Effect |
|---|---|---|
| SvxLink `STATS_AIORS_SAMPLE_INTERVAL` | 30 seconds | Collect AIORS diagnostics and update rolling history |
| SvxLink `STATS_INTERVAL` | 600 seconds | Write the full statistics snapshot, normally every 10 minutes |
| MQTT `POLL_INTERVAL_MS` | 1000 milliseconds | Check for a changed snapshot |

The agent queues telemetry only when the snapshot contents change. Faster
polling does not create new measurements. Locally retained configuration may
use different intervals; check `/etc/svxlink/svxlink.conf` and
`/etc/aiors-mqtt/agent.conf` before choosing alert thresholds.

`metrics.ts_unix_s` is the full snapshot time.
`metrics.aiors_sample_ts_unix_s` identifies its last successful AIORS sample;
`aiors_sample_ok`, `aiors_sample_error`, and `aiors_sample_age_s` describe
collection success and freshness. A collection failure can leave the last
successful radio states visible. The outer `published_at` records creation
of the MQTT message before it enters the queue, so it can be older than broker
delivery. Store server receipt time separately.

## Reporting variable reference

Checked on 7 November 2026 against the MQTT agent, AIORS diagnostics code and
SvxLink snapshot formatter. This reference covers the baseline 361 metric names in the
shared snapshot and legacy-primary aliases, plus the MQTT message fields. Per-logic additions are listed in the two-logic section. Older installed binaries can
report fewer fields. Related names are grouped using placeholders; each allowed
replacement is stated beside its table.

### Reading values, units and suffixes

Telemetry measurements are inside `metrics`. JSON carries the value without
a unit label: `13.72` in a voltage field means 13.72 V. The server can display
`13.72 V`, `0.80 A`, `10.96 W`, `22.5 °C` or `50 %RH`. Decimal formatting is a
display choice; the number of printed decimal places does not establish sensor accuracy.

| Variable | What it reports / units and values |
|---|---|
| V / `_v` | Volts: electrical voltage at the named measurement point. |
| A / `_a` | Amperes (amps): electrical current. 0.8 A = 800 mA. In PSU names, the separate channel letter a or b identifies the PSU. |
| W / `_w` | Watts: electrical power. These PSU fields are calculated input power, not transmitted RF power. |
| Wh / `_wh_total` | Watt-hours: accumulated energy. 1 Wh = 1 W for one hour; 1000 Wh = 1 kWh. |
| Ah / `_ah_total` | Ampere-hours: accumulated charge. This is not a remaining-battery-capacity reading. |
| °C / `_c` | Degrees Celsius at the environmental sensor. |
| %RH / `_rh` | Relative humidity percentage: 50 means 50 %RH, not 0.50. |
| s / `_s` | Seconds. A duration of 180 s can be displayed as 3 min; its transmitted value remains seconds. |
| ms / `_ms` | Milliseconds: 1000 ms = 1 s. |
| h / `_hours_total` | Hours as a decimal: 1.5 means 1 h 30 min. |
| `_duty_1h` | Percent of a 3600-second hour, from 0 to 100. A value of 25 means 25%, not 0.25. |
| `_bytes_1h` | Bytes counted at the FRN voice packet hooks; these are not total network-interface traffic. |
| `_evt`, `_samples`, `_n` | Counts: events/transitions, diagnostic observations, or valid measurements. They are not seconds. |
| `_min_s_1h` / `_max_s_1h` | Minimum / maximum completed session length in seconds. Here min means minimum, not minutes. |
| `_1h` / `_24h` | History over approximately the last hour / last 24 hours. The suffix is the window, not the unit of the value. |
| `_total` | Cumulative observed total, normally persisted by SvxLink. It survives restart when the totals file is preserved; clearing that file resets it. |
| `null`, `unknown`, `-1` | Unavailable values are not zero. null comes from snapshot na. Text unknown means no known state. Age fields use -1 when no event/sample has yet been observed. |

Keys containing `status`, `state`, `version`, `revision`, `scope`, `error`,
`reason` or `boot_id` are JSON strings, even when their values look numeric.
This includes `aiors_state_coverage_s_1h` and
`aiors_psu_a_state_change_evt_total`: convert a valid numeric string before
charting it. Most other numeric metrics are JSON numbers; `aiors_enabled` and
`aiors_sample_ok` are numeric 0/1. The separate `commands_enabled` field is a
JSON true/false boolean.

### MQTT message identity and time

| Variable | What it reports / units and values |
|---|---|
| `schema` | MQTT envelope format version, currently 1; no physical unit. |
| `kind` | Message type: telemetry, status, event or command_response. |
| `node_id` | Normalized logical node identity, for example 327CRMCE0; not a measurement. |
| `topic_node_id` | Topic-safe form of node_id. Unsafe topic characters are percent-encoded. |
| `boot_id` | Linux boot UUID. It changes on a Pi reboot; it is not a reboot counter. |
| `sequence` | Persistent message sequence number for telemetry, events and command responses. Status messages do not contain it. Gaps do not represent a duration. |
| `published_at` | UTC ISO 8601 timestamp ending in Z, assigned when this payload is created, before queued delivery. It is not necessarily sensor or server-receipt time. |
| `logic` | Telemetry only: Primary logic used for the temporary flat metric aliases; the `logics` object contains all scoped logic statistics. |
| `source` | Telemetry only: local snapshot file path, normally /var/lib/svxlink/svxstats_snapshot.txt. |
| `metrics` | Telemetry only: object holding the reporting variables described below. |

### MQTT agent status and queue

| Variable | What it reports / units and values |
|---|---|
| `state` | online or offline: MQTT agent connectivity. online does not prove that SvxLink or the radio hardware is healthy. |
| `reason` | connected, connection_lost or service_stopped: why this status was produced. |
| `agent_version`, `agent_revision` | MQTT agent build/Git identifiers; text, no physical unit. |
| `commands_enabled` | true allows configured remote commands; false means commands are disabled. Telemetry still works. |
| `queued_messages` | Number of locally queued outgoing messages, including one awaiting acknowledgment. |
| `queued_bytes` | Sum of queued topic and payload lengths in bytes; not the SQLite database file size. |
| `queue_message_limit` | Configured maximum queue depth, in messages. |
| `queue_byte_limit` | Configured queue size limit, in bytes accounted as above. |

Status is published on connection changes, rather than with every sensor
measurement. Queue figures describe the moment that status payload was built.
The broker's offline Last Will payload is prepared in advance, so its
`published_at` is not an exact disconnect timestamp.

### SvxLink snapshot identity and uptime

| Variable | What it reports / units and values |
|---|---|
| `stats_schema` | Snapshot format version, currently 3; no physical unit. |
| `ts_unix_s` | Full snapshot creation time, seconds since 1970-01-01 00:00:00 UTC (Unix time). |
| `uptime_s` | Seconds since the current SvxLink process started; alias of svxlink_uptime_s. |
| `linux_uptime_s` | Seconds since Linux boot; -1 if unavailable. Separate from SvxLink runtime. |
| `svxlink_uptime_s` | Seconds since the current SvxLink process started. |
| `svxlink_software_version` | SvxLink software version identifier; text. |
| `svxlink_software_revision` | SvxLink build/revision identifier; text. |
| `linux_boot_id` | Linux boot UUID copied into the snapshot; text, not a count. |

### FRN connection, activity and users

| Variable | What it reports / units and values |
|---|---|
| `frn_link_up_evt_1h` | Number of FRN link-up events in the last hour. |
| `frn_link_down_evt_1h` | Number of FRN link-down events in the last hour. |
| `frn_link_up_total` | Cumulative count of observed FRN link-up events. |
| `frn_link_down_total` | Cumulative count of observed FRN link-down events. |
| `frn_link_up_age_s` | Seconds since the most recent link-up event; -1 if none observed in this process. |
| `frn_link_down_age_s` | Seconds since the most recent link-down event; -1 if none observed in this process. |
| `frn_rx_age_s` | Seconds since recorded FRN receive activity, including client-list updates as well as voice data. Not exclusively time since speech; -1 if none. |
| `frn_tx_age_s` | Seconds since recorded outgoing FRN voice activity; -1 if none. |
| `frn_users` | Current count of distinct entries in the FRN client list; not necessarily distinct people. |
| `frn_users_peak_1h` | Tracked peak client count. The implementation resets it roughly hourly; it is not an exact sliding-hour maximum. |
| `frn_users_unique_1h` | Count of distinct FRN client-list identities seen during the last hour. |
| `frn_link_up_1h` | Compatibility alias of frn_link_up_evt_1h; same count, do not add them together. |
| `frn_link_down_1h` | Compatibility alias of frn_link_down_evt_1h; same count, do not add them together. |
| `frn_user_join_1h` | Count of entries added between observed FRN client lists in the last hour. |
| `frn_user_leave_1h` | Count of entries removed between observed FRN client lists in the last hour. |

The up/down ages are event ages, not a connection-state boolean. Both can
be positive. A reconnect can affect client-list join/leave counts without
representing a person physically arriving or leaving.

### FRN voice traffic

For the patterns below, replace `<dir>` with `tx` (audio sent from this node
to FRN) or `rx` (audio received from FRN).

| Variable | What it reports / units and values |
|---|---|
| `frn_<dir>_evt_1h` | Completed voice sessions in the last hour; count. |
| `frn_<dir>_s_1h` | Voice-session duration in the last hour, in seconds; includes the current active session contribution. |
| `frn_<dir>_min_s_1h` | Shortest completed voice session, in seconds; 0 when none is recorded. |
| `frn_<dir>_max_s_1h` | Longest completed voice session, in seconds; 0 when none is recorded. |

| Variable | What it reports / units and values |
|---|---|
| `frn_tx_bytes_1h` | Outgoing GSM voice payload bytes in the last hour; excludes FRN control commands and TCP/IP overhead. |
| `frn_rx_bytes_1h` | Received voice packet bytes including its FRN client index; excludes general client lists and TCP/IP overhead. Not exactly the same accounting as TX. |
| `frn_tx_duty_1h` | Outgoing FRN voice duration / 3600 × 100, clipped to 0–100%; 10 means 10%. |

There is no exported `frn_rx_duty_1h` metric in this snapshot. The textual
CLI report can contain fields that are not in MQTT.

### RF transmitter and receiver activity

Replace `<dir>` with `tx` or `rx`. TX measures SvxLink transmitter-on
state, including announcements, linked audio and chat playback. RX measures
receiver squelch-open state, not a speech detector. These are SvxLink statistics,
not separate AI/BI/AE/BE physical-port counters.

| Variable | What it reports / units and values |
|---|---|
| `rf_<dir>_evt_1h` | Completed transmitter-on / squelch-open periods in the last hour; count. |
| `rf_<dir>_s_1h` | Accumulated active time in the last hour, in seconds; includes the ongoing period. |
| `rf_<dir>_min_s_1h` | Shortest completed active period in the last hour, in seconds; 0 when none. |
| `rf_<dir>_max_s_1h` | Longest completed active period in the last hour, in seconds; 0 when none. |
| `rf_<dir>_hours_total` | Cumulative active time in decimal hours; includes the ongoing period. |
| `rf_<dir>_duty_1h` | Active time / 3600 × 100, clipped to 0–100%; 25 means 15 minutes active per full hour. |

FRN, RF and RunCmd one-hour statistics use 60 minute buckets, so the
boundary is approximate. Completed-session counts and minimum/maximum durations
are recorded when the session ends. Live duration can appear before that count
increases. Duty always uses a full-hour denominator, even soon after startup.

### FRN RunCmd command statistics

These describe the FRN RunCmd-to-aiorsctl bridge, separately from MQTT
remote-command responses.

| Variable | What it reports / units and values |
|---|---|
| `cmd_ok_1h` | Accepted FRN RunCmd requests in the last hour; count. Acceptance does not guarantee the external command exited successfully. |
| `cmd_rej_1h` | Rejected RunCmd requests in the last hour; count. |
| `cmd_authfail_1h` | RunCmd authentication failures in the last hour; count. |
| `cmd_bc_attempt_1h` | Broadcast/non-private RunCmd attempts that were ignored; count. |
| `cmd_avg_ms_1h` | Mean measured external-command elapsed time in milliseconds; integer average, 0 if no timings. |
| `cmd_max_ms_1h` | Maximum measured external-command elapsed time in milliseconds; 0 if no timings. |

### AIORS collection, identity and freshness

| Variable | What it reports / units and values |
|---|---|
| `aiors_enabled` | 1 = SvxLink AIORS diagnostics collection enabled; 0 = disabled. |
| `aiors_sample_ok` | 1 = latest collection/parse attempt succeeded; 0 = it did not. A successful sample can still report hardware faults. |
| `aiors_sample_error` | Collector result text, such as none on success, not_started before collection, or a timeout/exit/parse error. |
| `aiors_sample_age_s` | Seconds since the last successful diagnostics sample; -1 if none. |
| `aiors_attempt_age_s` | Seconds since the last collection attempt, successful or not; -1 if none. |
| `aiors_sample_ts_unix_s` | Last successful sample time, Unix UTC seconds; 0 if no successful sample exists. |
| `aiors_diagnostics_version` | AIORS diagnostics format version, currently string "5"; schema "4" also accepted. |
| `aiors_software_version` | Running AIORS daemon Git/build description, text. |
| `aiors_software_revision` | Running AIORS daemon Git/build revision; currently the same identity as aiors_software_version. |
| `aiors_hardware_version` | AIORS board version string "1.0" or "1.1"; not a voltage. |
| `aiors_uptime_s` | AIORS daemon uptime in seconds. Separate from Linux uptime and SvxLink uptime. |
| `aiors_psu_current_scope` | per_channel = independent A/B current readings on HW 1.1; common = shared current on HW 1.0; unknown = identity unavailable. |

The current AIORS values below come from the last successful sample. They
can remain visible after a collection failure. Check `aiors_sample_ok`, sample
age and both timestamps before displaying them as current. If SvxLink stops,
the independent MQTT agent can remain online with an old snapshot. Use server
receipt time and measurement time to detect this; an unchanged stored age value
does not keep counting on the server by itself.

### AIORS board and power-supply states

| Variable | What it reports / units and values |
|---|---|
| `aiors_overall_status` | ok = monitored components ready and both PSUs on; fault = a reported PSU hardware fault; degraded = another monitored requirement is not satisfied. This is the daemon health summary. |
| `aiors_psu_gpio_status` | ok / error: whether PSU enable/fault GPIO states could be read. |
| `aiors_adc_status` | ok / error: combined health of the required analog-to-digital converters. |
| `aiors_adc_primary_status` | ok / error: primary ADC measurement availability. |
| `aiors_adc_secondary_status` | ok / error for the secondary ADC on HW 1.1; null when absent/not applicable on HW 1.0. |
| `aiors_fan_state` | on / off: fan control GPIO state. null if unavailable; it does not measure fan RPM or prove the fan is turning. |
| `aiors_environment_status` | ok / error: availability of temperature/humidity readings from the environmental sensor. |
| `aiors_digital_inputs_status` | ok / error: whether the four digital input lines could be read. |

In the next table, `<ch>` is `a` or `b`, identifying PSU A or PSU B.

| Variable | What it reports / units and values |
|---|---|
| `aiors_psu_<ch>_state` | on / off / unknown: PSU enable GPIO readback. It is not a voltage-threshold alarm. |
| `aiors_psu_<ch>_fault` | yes / no / unknown: the PSU hardware fault indication. yes means the fault input is asserted; it is not a numeric fault code. |

| Variable | What it reports / units and values |
|---|---|
| `aiors_di<input>` | Digital input DI0–DI3: on / off is the GPIO logical input state; unknown means a read failure. No voltage unit. Its real-world purpose depends on external wiring. |

For `aiors_di<input>`, replace `<input>` with `0`, `1`, `2` or `3`, for
example `aiors_di2`. These inputs do not inherently mean door, battery or alarm;
assign those labels only when the installation wiring defines them.

### Physical radios and USB audio

| Variable | What it reports / units and values |
|---|---|
| `aiors_radio_ai_status` | Physical internal radio A lifecycle state; see values below. |
| `aiors_radio_bi_status` | Physical internal radio B lifecycle state; see values below. |
| `aiors_radio_ae_status` | Physical external radio A lifecycle state; see values below. |
| `aiors_radio_be_status` | Physical external radio B lifecycle state; see values below. |
| `aiors_sa818_a_status` | Legacy internal radio A health: ok / fault; schema 5 uses null for disabled/unimplemented. It overlaps physical AI, rather than adding another radio. |
| `aiors_sa818_b_status` | Legacy internal radio B health: ok / fault; schema 5 uses null for disabled/unimplemented. It overlaps physical BI. |
| `aiors_cm108_a_status` | USB audio A: ok when its configured audio card can be found, otherwise fault. This does not prove the complete RF audio/PTT path works. |
| `aiors_cm108_b_status` | USB audio B: ok when its configured audio card can be found, otherwise fault. Same limitation as A. |

| Physical radio state | Meaning |
|---|---|
| `ready` | Radio identified and configured. |
| `disabled` | Radio management disabled; not a radio fault. |
| `unimplemented` | External port has no implemented driver; not counted as a radio fault. |
| `absent` | Expected radio not responding; counted as a fault. |
| `mismatch` | Detected radio identity differs from configuration; counted as a fault. |
| `config_error` | Radio configuration/verification failed; counted as a fault. |
| `uninitialized` | Radio has not reached an initialized state; counted as a fault. |
| `null` | No physical lifecycle value available, including schema 4; not a measured healthy or faulty state. |

### Electrical and environmental measurements

| Variable | What it reports / units and values |
|---|---|
| `aiors_psu_a_input_voltage_v` | V: monitored PSU A input supply voltage, already scaled to the input measurement point. |
| `aiors_psu_a_input_current_a` | A: PSU A input current on HW 1.1; shared/common current on HW 1.0 (see warning below). |
| `aiors_psu_a_input_power_w` | W: calculated PSU A electrical input power = input voltage × input current; valid for per-channel HW 1.1 measurements. |
| `aiors_psu_b_input_voltage_v` | V: monitored PSU B input supply voltage, already scaled to the input measurement point. |
| `aiors_psu_b_input_current_a` | A: PSU B input current on HW 1.1; same shared/common current as A on HW 1.0. |
| `aiors_psu_b_input_power_w` | W: calculated PSU B electrical input power = input voltage × input current; valid for per-channel HW 1.1 measurements. |
| `aiors_bus_3v3_voltage_v` | V: measured voltage of the nominal 3.3 V board supply bus, not a fixed constant. |
| `aiors_bus_5v_voltage_v` | V: measured voltage of the nominal 5 V board supply bus, not a fixed constant. |
| `aiors_rf_a_average_voltage_v` | V: RF A average-voltage monitor signal at the ADC input on HW 1.1. No conversion to watts, dBm or SWR is provided. |
| `aiors_rf_b_average_voltage_v` | V: RF B average-voltage monitor signal at the ADC input on HW 1.1. It is not a calibrated RF power reading. |
| `aiors_temperature_c` | °C: environmental sensor temperature on the AIORS board; not the Pi CPU temperature. |
| `aiors_humidity_rh` | %RH: environmental sensor relative humidity; 50 means 50%. |
| `aiors_ai0_voltage_v` | V: external analog input AI0 voltage after the configured board divider scaling. Its attached sensor/function depends on wiring. |
| `aiors_ai1_voltage_v` | V: external analog input AI1 voltage, scaled as above. |
| `aiors_ai2_voltage_v` | V: external analog input AI2 voltage, scaled as above. |
| `aiors_ai3_voltage_v` | V: external analog input AI3 voltage on HW 1.1, scaled as above; unavailable on HW 1.0. |

These numbers are engineering values, not raw ADC counts. Do not apply the
board divider factor again. On HW 1.0, RF monitor voltages and AI3 are not
available, so their current measurements are null. HW 1.0 reports the same common
current under both PSU current names: do not add A and B and double-count it.
Per-channel input power and new per-channel energy/charge integration are not
available from that common-current measurement.

### Minimum, maximum, average and sample-count fields

Every one of the 16 measurement names in the previous table also has the
following eight fields. Replace `<base>` with the complete measurement name,
including its unit suffix.

| Variable | What it reports / units and values |
|---|---|
| `<base>_min_1h` | Smallest valid sample in the last hour; same unit as the base measurement. |
| `<base>_max_1h` | Largest valid sample in the last hour; same unit as the base measurement. |
| `<base>_avg_1h` | Arithmetic mean of valid samples in the last hour; same unit as the base measurement. |
| `<base>_n_1h` | Number of valid samples contributing to that hour; unitless count. |
| `<base>_min_24h` | Smallest valid sample in the last 24 hours; same unit as the base measurement. |
| `<base>_max_24h` | Largest valid sample in the last 24 hours; same unit as the base measurement. |
| `<base>_avg_24h` | Arithmetic mean of valid samples in the last 24 hours; same unit as the base measurement. |
| `<base>_n_24h` | Number of valid samples contributing to those 24 hours; unitless count. |

Example: `aiors_psu_a_input_voltage_v_avg_1h=13.72` means mean PSU A input
voltage 13.72 V, and `aiors_psu_a_input_voltage_v_n_1h=360` means 360 valid
voltage samples contributed. Averages are sample-weighted, not time-weighted.
Invalid samples are excluded; with no valid samples, n is 0 and min/max/avg
are null. Rolling history starts again when SvxLink restarts, so a 24-hour field
does not imply 24 hours of coverage immediately after startup.

### AIORS diagnostic counts and observed state time

For this table, replace `<window>` with `total`, `1h` or `24h`.
Counts are observations by SvxLink, not an independent hardware log.

| Variable | What it reports / units and values |
|---|---|
| `aiors_samples_ok_<window>` | Count of successfully collected and parsed diagnostics samples, regardless of whether the hardware was healthy. |
| `aiors_samples_failed_<window>` | Count of failed diagnostics collection/parse attempts; not a count of hardware faults. |
| `aiors_state_coverage_s_<window>` | Seconds of observed/estimated state coverage. Numeric-looking JSON string because the name contains state; not necessarily full wall-clock runtime. |
| `aiors_restart_evt_<window>` | Detected AIORS daemon restarts, identified by a decrease in sampled daemon uptime; count. Not a Pi reboot count and can miss restarts between observations. |
| `aiors_degraded_samples_<window>` | Count of successful diagnostics samples whose overall status was degraded. |
| `aiors_fault_samples_<window>` | Count of successful diagnostics samples whose overall status was fault. |
| `aiors_degraded_s_<window>` | Estimated seconds spent in overall degraded status during covered intervals. |
| `aiors_fault_s_<window>` | Estimated seconds spent in overall fault status during covered intervals. |

### PSU transitions, on/off time, charge and energy

In the next table, `<ch>` is `a` or `b` and `<window>` is `total`, `1h`
or `24h`. A transition event is counted when a change is observed between samples.

| Variable | What it reports / units and values |
|---|---|
| `aiors_psu_<ch>_state_change_evt_<window>` | Count of observed PSU on/off state changes. Numeric-looking JSON string because the name contains state. |
| `aiors_psu_<ch>_off_s_<window>` | Estimated PSU-off time in seconds during covered intervals. |
| `aiors_psu_<ch>_fault_evt_<window>` | Count of observed transitions into an asserted PSU hardware fault. |
| `aiors_psu_<ch>_fault_s_<window>` | Estimated seconds with the PSU hardware fault asserted. |

The following per-channel values exist only as cumulative totals;
replace `<ch>` with `a` or `b`. There are no exported 1h/24h variants of these rows.

| Variable | What it reports / units and values |
|---|---|
| `aiors_psu_<ch>_on_s_total` | Estimated cumulative seconds with that PSU enabled during covered intervals. |
| `aiors_psu_<ch>_energy_coverage_s_total` | Cumulative seconds with sufficient valid measurements to integrate that channel’s energy and charge. |
| `aiors_psu_<ch>_energy_wh_total` | Cumulative electrical input energy in Wh, integrated from sampled channel power. Not RF output energy; null until integration coverage exists. |
| `aiors_psu_<ch>_charge_ah_total` | Cumulative input charge in Ah, integrated from sampled channel current. Not battery capacity or charge remaining; null until integration coverage exists. |

| Variable | What it reports / units and values |
|---|---|
| `aiors_energy_coverage_s_total` | Cumulative seconds in which both PSU channels had valid energy-integration coverage. It is not the sum of the A and B coverage durations. |

For example, 10 W measured continuously for one hour adds approximately
10 Wh; 0.8 A for one hour adds approximately 0.8 Ah. Integration uses adjacent
valid samples and their elapsed time. Gaps beyond the permitted continuity limit
(three configured sample intervals) and detected daemon restarts interrupt
cumulative integration; unknown time is not automatically counted as normal operation.
State durations are sampled estimates: transitions between samples cannot be
timed exactly. Rolling state coverage also limits how long a last state is held.

### Radio and USB fault histories

Choose exactly one of these eight `<device>` prefixes, then append a
suffix from the following table. Replace `<window>` with `total`, `1h` or `24h`.

| Device prefix | What it identifies |
|---|---|
| `aiors_radio_ai` | Physical internal radio A; use this for new dashboards. |
| `aiors_radio_bi` | Physical internal radio B; use this for new dashboards. |
| `aiors_radio_ae` | Physical external radio A. |
| `aiors_radio_be` | Physical external radio B. |
| `aiors_sa818_a` | Legacy internal radio A history; overlaps AI in diagnostics schema 5. |
| `aiors_sa818_b` | Legacy internal radio B history; overlaps BI in diagnostics schema 5. |
| `aiors_cm108_a` | USB audio device A availability history. |
| `aiors_cm108_b` | USB audio device B availability history. |

| Variable | What it reports / units and values |
|---|---|
| `<device>_fault_evt_<window>` | Count of observed transitions into a fault state. A first-ever faulty sample can add a sample without adding a transition. |
| `<device>_fault_samples_<window>` | Count of successful diagnostics samples reporting that device faulty. One continuous fault can produce many fault samples but only one transition event. |
| `<device>_fault_s_<window>` | Estimated seconds in a fault state during covered intervals. |

Example: `aiors_radio_ae_fault_evt_24h=2` means two observed transitions
into an external-A radio fault during the last day. Disabled and unimplemented
physical ports accumulate no faults. Do not add legacy SA818 A/B counters to
physical AI/BI counters as though they were separate devices. A zero count alone
does not establish that the device was observed and healthy; check its current
status, sample freshness and coverage.

### Change events and optional command responses

| Variable | What it reports / units and values |
|---|---|
| `event` | For current change events: metric_changed. |
| `metric` | Exact name of the monitored metric that changed. |
| `previous`, `current` | Before/after values as strings; units and meanings are inherited from that metric. Snapshot na is the string "na" here, not JSON null. |
| `request_id` | Command response: request identity used to correlate the result; no unit. |
| `operation` | Command response: operation name, for example fan.set. |
| `ok` | Command response: boolean true for a successful execution result; false for a failure or rejection. |
| `exit_code` | Executed-command response: process exit status number; 0 is success, nonzero indicates failure. Not a duration. |
| `timed_out` | Executed-command response: true if execution exceeded its configured time limit. |
| `output` | Executed-command response: bounded text output from the local command. |
| `error` | Rejected-command response: text explaining rejection/unknown outcome; execution-only fields need not be present. |

Events are produced for selected watched metrics when the agent sees a
changed snapshot, not for every variable or every physical edge. The first
observation initializes comparison state without an event. The Events and Remote
Commands sections below describe the watched fields and command policy.

### Choosing dashboard labels and alerts

Display a unit next to every measurement and use separate labels for
instantaneous voltage/current/power, accumulated energy/charge, sample counts and
durations. Treat null as unavailable, retain an explicit stale indication, and
avoid replacing missing data with zero. Lifetime totals need their persistence
file; rolling histories are process-local. Ratios and availability percentages
should use the reported coverage where applicable, rather than assuming that a
full hour or day was observed. Actual analog accuracy and alarm thresholds depend
on the installed board, sensor calibration and wiring; no universal safe voltage
or temperature limits are implied by these field names.

## Software and hardware identity

AIORS software versions are build-time Git descriptions, such as `4f165e5` or
`4f165e5-dirty`. The separately maintained release number has been removed.
Descriptions can also be tag-based; `-dirty` indicates uncommitted build
changes. `unknown` means Git metadata was unavailable and no `GIT_VERSION`
override was supplied. Treat these identifiers as opaque strings, including
hashes containing only digits; do not parse or order them as release numbers.

- MQTT status retains `agent_version` and `agent_revision`; both contain the
  MQTT agent's Git description.
- Telemetry retains `metrics.aiors_software_version` and
  `metrics.aiors_software_revision`; both contain the running AIORS daemon's
  Git description, sampled by SvxLink.
- `metrics.aiors_hardware_version` remains `"1.0"` or `"1.1"`.
- SvxLink's own `svxlink_software_version` and `svxlink_software_revision`
  fields retain their existing formats.

The agent and daemon are separate binaries and can report different revisions
after partial upgrades. Unavailable telemetry identity values become JSON
`null` through the normal `na` conversion. Available identity values remain
JSON strings.

Use `aiorsd --version` and `aiorsctl --version` to identify the installed
binaries, `aiorsctl version` to query the running daemon, and
`aiors-mqtt-agent --version` to identify the independent agent. Example output:

```text
aiorsd 4f165e5-dirty (hw 1.0)
aiorsctl 4f165e5-dirty (hw 1.0)
aiorsd 4f165e5-dirty (hw 1.0)
aiors-mqtt-agent 4f165e5-dirty
```

FRN `get version` forwards the daemon identity alongside SvxLink's own version.
The agent has no hardware suffix. Commit source changes before release builds;
the commit alone does not identify all changes in a dirty build.

MQTT envelope `schema=1`, topic prefix `frn/aiors/v1`, and SvxLink snapshot
`stats_schema=4` includes the named logic blocks and retains the legacy primary aliases. Current AIORS diagnostics use schema 5; the
updated SvxLink reader also supports schema 4. MQTT exports the current value
as `"aiors_diagnostics_version": "5"` because keys containing `version` remain
strings. Adding the physical radio fields does not change MQTT topics or its
envelope schema. Deploy matching updated SvxLink and MQTT-agent binaries to
obtain all four radio states and their change events.

## Events

The agent emits `metric_changed` events for these state fields:

```text
frn_link_up_total
frn_link_down_total
aiors_sample_ok
aiors_overall_status
aiors_psu_gpio_status
aiors_psu_a_state
aiors_psu_a_fault
aiors_psu_b_state
aiors_psu_b_fault
aiors_adc_status
aiors_environment_status
aiors_sa818_a_status
aiors_sa818_b_status
aiors_radio_ai_status
aiors_radio_bi_status
aiors_radio_ae_status
aiors_radio_be_status
aiors_cm108_a_status
aiors_cm108_b_status
aiors_restart_evt_total
```

Example:

```json
{
  "schema": 1,
  "kind": "event",
  "node_id": "S5RPTAIORS",
  "topic_node_id": "S5RPTAIORS",
  "boot_id": "42f81f04-66e6-4692-8dde-81a8d8f7e19b",
  "sequence": 1843,
  "published_at": "2026-11-03T12:31:00Z",
  "event": "metric_changed",
  "metric": "aiors_radio_ai_status",
  "previous": "ready",
  "current": "absent"
}
```

The first observed value seeds local state and is not reported as a change.
The baseline is kept in SQLite across agent restarts. Repeated identical
values create no duplicate change event. Event `previous` and `current` values
are strings, including `na`, while the corresponding telemetry value for
`na` is JSON `null`.

Events describe changes observed in exported snapshots. A transition that
starts and ends between snapshots can be missed by the event stream, even
when SvxLink's more frequent AIORS samples affect its rolling fault counters.

## Pi Configuration

The combined installer creates a disabled configuration when none exists and
preserves an existing configuration on upgrades. To create a missing file
manually from the managed template, then edit it:

```sh
if ! sudo test -f /etc/aiors-mqtt/agent.conf; then
  sudo install -o root -g aiors-mqtt -m 0640 \
    /usr/local/share/aiors/aiors-mqtt-agent.conf \
    /etc/aiors-mqtt/agent.conf
fi
sudoedit /etc/aiors-mqtt/agent.conf
```

Set at minimum:

```ini
[agent]
ENABLED=1

[broker]
HOST=mqtt.example.net
PORT=8883
USERNAME=S5RPTAIORS
PASSWORD_FILE=/etc/aiors-mqtt/password
TLS=1
CA_FILE=/etc/aiors-mqtt/ca.crt
```

Install secrets with restrictive permissions:

```sh
sudo install -o root -g aiors-mqtt -m 0640 password \
  /etc/aiors-mqtt/password
sudo install -o root -g aiors-mqtt -m 0644 ca.crt \
  /etc/aiors-mqtt/ca.crt
```

Mutual TLS is supported with `CERT_FILE` and `KEY_FILE`; set both or neither.
Validate and start:

```sh
sudo -u aiors-mqtt /usr/local/sbin/aiors-mqtt-agent --check-config \
  -c /etc/aiors-mqtt/agent.conf
sudo systemctl enable --now aiors-mqtt-agent
systemctl status aiors-mqtt-agent --no-pager
journalctl -u aiors-mqtt-agent -b --no-pager -n 100
```

`--check-config` checks configuration and local file access. It does not test
a broker connection or prove that credentials and TLS authentication work;
confirm those in the service log and by receiving messages on the server.

Relevant managed defaults are:

| Setting | Default |
|---|---|
| Agent and remote commands `ENABLED` | Both 0 until explicitly configured |
| `NODE_ID` | Empty; derive from SvxLink callsign |
| `POLL_INTERVAL_MS` | 1000 |
| `MAX_QUEUE_MESSAGES` | 10080 |
| `MAX_QUEUE_BYTES` | 52428800, or 50 MiB of accounted outbox content |
| Broker `PORT` / `TLS` / `KEEPALIVE_S` | 8883 / 1 / 30 seconds |
| Broker `TOPIC_PREFIX` | `frn/aiors/v1` |
| Commands `TIMEOUT_MS` | 3000; accepted range 100-30000 milliseconds |

For a fresh combined deployment, private files can be supplied without adding
them to Git:

```sh
MQTT_CONFIG_PATH=$HOME/private/agent.conf \
MQTT_CA_PATH=$HOME/private/ca.crt \
MQTT_PASSWORD_PATH=$HOME/private/password \
AIORS_HW_VERSION=1.1 \
DEPLOY_VERSION=deploy-YOUR_RELEASE \
  bash install.sh
```

Existing MQTT config and credentials are preserved on upgrades when these
variables are omitted.
Replace `deploy-YOUR_RELEASE` with the deployment release tag you intend to
install. `MQTT_CERT_PATH` and `MQTT_KEY_PATH` can also supply mutual-TLS files.

## S59RKS: step-by-step setup from a Windows PC

Follow this walkthrough to activate the existing MQTT agent on the Raspberry
Pi. It uses the S59RKS connection details supplied for Mrzlica. The transfer
folder is `s59rks-mqtt`. Commands are labelled with the computer on which they
must run. Only the MQTT agent is restarted during this procedure.

| Setting | Value |
|---|---|
| Broker hostname | `s59rks.si` |
| TCP port | `8883` |
| Username and MQTT node ID | `327CRMCE0` |
| Topic prefix | `frn/aiors/v1` |
| Authentication | MQTT password; verify the server with the supplied CA |
| Remote commands | Disabled |

Before starting, obtain `ca.crt` and `327CRMCE0.password` from the broker
administrator. The CA certificate does not replace the MQTT password. Keep
the password out of chat, screenshots and command-line arguments. No client
certificate or private key is required for this connection.

### Step 1 - On the Pi: check the installed agent

```sh
id aiors-mqtt
/usr/local/sbin/aiors-mqtt-agent --version
sudo systemctl status aiors-mqtt-agent --no-pager
```

An inactive or disabled service is expected before activation. If the user,
executable or service is missing, stop here and check the AIORS installation
before proceeding.

Check that the service account can read its local inputs:

```sh
sudo -u aiors-mqtt test -r /etc/svxlink/svxlink.conf \
  && echo "SvxLink configuration readable"

sudo -u aiors-mqtt test -r /var/lib/svxlink/svxstats_snapshot.txt \
  && echo "Statistics snapshot readable"
```

Both confirmation messages should appear. If the snapshot is missing or not
readable, resolve statistics generation or permissions before continuing.

### Step 2 - On the Pi: create the transfer folder

```sh
install -d -m 700 "$HOME/s59rks-mqtt"
hostname -I
```

Note the Pi's LAN IP address for the next step. This walkthrough assumes the
Pi login is `aiors` and its home directory is `/home/aiors`.

### Step 3 - On Windows: copy the files with PowerShell

Place `ca.crt` and `327CRMCE0.password` in your Windows Downloads folder.
Open PowerShell on the Windows PC, then run these commands. Replace `PI_IP`
with the Pi's actual IP address, using the same address as for SSH.

```powershell
scp "$env:USERPROFILE\Downloads\ca.crt" "aiors@PI_IP:/home/aiors/s59rks-mqtt/"
```

```powershell
scp "$env:USERPROFILE\Downloads\327CRMCE0.password" "aiors@PI_IP:/home/aiors/s59rks-mqtt/"
```

If your files are elsewhere, change the source paths accordingly. When prompted
for a password, enter the Pi login password for `aiors`, not the MQTT password
inside the transferred file.

### Step 4 - Back on the Pi: install the certificate and password

```sh
cd "$HOME/s59rks-mqtt"
chmod 600 327CRMCE0.password

sudo install -d -o root -g aiors-mqtt -m 0750 /etc/aiors-mqtt

sudo install -o root -g aiors-mqtt -m 0640 \
  327CRMCE0.password /etc/aiors-mqtt/password

sudo install -o root -g aiors-mqtt -m 0644 \
  ca.crt /etc/aiors-mqtt/ca.crt
```

Copy the password file as supplied, without adding quotes or spaces to its
contents. The service reads the installed copies in `/etc/aiors-mqtt`.

### Step 5 - On the Pi: configure the agent

Back up the existing configuration, or create it from the installed template:

```sh
if sudo test -f /etc/aiors-mqtt/agent.conf; then
  sudo cp -a /etc/aiors-mqtt/agent.conf \
    "/etc/aiors-mqtt/agent.conf.bak.$(date +%Y%m%d-%H%M%S)"
else
  sudo install -o root -g aiors-mqtt -m 0640 \
    /usr/local/share/aiors/aiors-mqtt-agent.conf \
    /etc/aiors-mqtt/agent.conf
fi

sudoedit /etc/aiors-mqtt/agent.conf
```

Change these entries within their existing sections. Keep the other settings
from the existing file; do not add duplicate sections or replace the entire
configuration with only this extract.

```ini
[agent]
ENABLED=1
NODE_ID=327CRMCE0
SVXLINK_CONFIG=/etc/svxlink/svxlink.conf
SNAPSHOT_PATH=/var/lib/svxlink/svxstats_snapshot.txt
POLL_INTERVAL_MS=1000

[broker]
HOST=s59rks.si
PORT=8883
USERNAME=327CRMCE0
PASSWORD_FILE=/etc/aiors-mqtt/password
TLS=1
CA_FILE=/etc/aiors-mqtt/ca.crt
TOPIC_PREFIX=frn/aiors/v1
CERT_FILE=
KEY_FILE=

[commands]
ENABLED=0
```

Use the hostname without `https://` or a URL path. `NODE_ID` is the MQTT
identity; no FRN account or radio callsign change is needed.

Ensure the service can read the configuration:

```sh
sudo chown root:aiors-mqtt /etc/aiors-mqtt/agent.conf
sudo chmod 0640 /etc/aiors-mqtt/agent.conf
```

### Step 6 - On the Pi: test the TLS connection

```sh
sudo -u aiors-mqtt timeout 15 openssl s_client \
  -connect s59rks.si:8883 \
  -servername s59rks.si \
  -CAfile /etc/aiors-mqtt/ca.crt \
  -verify_hostname s59rks.si \
  -verify_return_error \
  -brief </dev/null
```

Look for `Verification: OK`. This checks reachability and the server certificate;
it does not test the MQTT username or password. If verification fails, check
the Pi's clock, hostname and CA file. For connection failures, check DNS and
outbound TCP access to port 8883. Keep certificate verification enabled.

### Step 7 - On the Pi: validate and activate

First validate the local configuration:

```sh
sudo -u aiors-mqtt /usr/local/sbin/aiors-mqtt-agent \
  --check-config -c /etc/aiors-mqtt/agent.conf
```

After a successful check, enable and start the service:

```sh
sudo systemctl enable aiors-mqtt-agent
sudo systemctl restart aiors-mqtt-agent

sudo systemctl status aiors-mqtt-agent --no-pager
sudo journalctl -u aiors-mqtt-agent -n 60 --no-pager
```

Look for `connected to MQTT broker`. To watch new log messages:

```sh
sudo journalctl -u aiors-mqtt-agent -f
```

Press Ctrl+C to leave the log viewer; the service continues running. The
configuration check alone does not prove broker authentication or delivery.

### Step 8 - Confirm fresh telemetry with the server administrator

Ask the administrator to run the server-side `watch.sh` and check these topics:

```text
frn/aiors/v1/327CRMCE0/status
frn/aiors/v1/327CRMCE0/telemetry
```

Expected status is `state=online` with `commands_enabled=false`. The node
account is publish-only; the server administrator uses the monitor account
to subscribe. Do not use the node account for a `mosquitto_sub` test.

Allow up to ten minutes for a new full statistics snapshot with the managed
`STATS_INTERVAL=600` default. Polling the file every second does not create
new measurements. Confirm that telemetry timestamps advance, since retained
messages can contain older data. The managed AIORS sampling interval is
`STATS_AIORS_SAMPLE_INTERVAL=30`; check the actual active settings if timing
differs.

On the Pi, inspect the snapshot timestamps and collection state with:

```sh
sudo -u aiors-mqtt cat /var/lib/svxlink/svxstats_snapshot.txt \
  | tr ' ' '\n' \
  | grep -E '^(ts_unix_s|aiors_sample_ts_unix_s|aiors_sample_ok|aiors_overall_status)='
```

The snapshot is one line; this command splits its fields for reading. Successful
activation requires the server to receive fresh telemetry as well as online
status. If a step fails, collect that command's output and the last 60 MQTT-agent
log lines, without including the password.

## Server Broker

Install Mosquitto on the FRN server and use TLS plus authentication. A minimal
configuration is provided in `docs/mosquitto-aiors.conf.example` and an ACL
template in `docs/mosquitto-aiors.acl.example`.

Create a distinct broker account for each node. The sample ACL allows a node
to publish only its status, telemetry, events, and responses, and to read only
its own command request topic. Give the server ingestion/control application a
separate account.

Verify from the server:

```sh
mosquitto_sub -h mqtt.example.net -p 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  -u frn-ingest -P 'SERVER_PASSWORD' \
  -t 'frn/aiors/v1/+/status' \
  -t 'frn/aiors/v1/+/telemetry' \
  -t 'frn/aiors/v1/+/events' -v
```

For queued telemetry, events, and command responses, the server database should
use `(node_id, boot_id, sequence)` as an idempotency key. Status messages have
no `sequence`; maintain the latest status per node separately. QoS 1 is at
least once, so duplicate delivery is valid. Store the metric
timestamp `metrics.ts_unix_s` as snapshot time, retain
`metrics.aiors_sample_ts_unix_s` for the AIORS observation time, and record both
the message creation time `published_at` and server receipt time. Alert on
retained `status.state=offline`, stale telemetry, an
increasing failure/fault counter, or threshold violations. Do not calculate a
new event by summing repeated retained snapshots; store counter samples and
derive deltas per node and boot.

Use the four explicit physical radio states for radio alarms. `disabled` and
`unimplemented` describe management availability and do not indicate a failed
managed module. Set telemetry freshness thresholds around the configured
snapshot interval; the current deployment template normally updates every
10 minutes. An online MQTT agent can still have stale SvxLink or AIORS data.

## Remote Commands

Remote commands are compiled and installed but `[commands] ENABLED=0` is the
default. When disabled, the agent does not subscribe to `command/request`.

Request example; its timestamp is illustrative. Generate expiration at send
time from the current clock, for example current Unix time plus 30 seconds:

```json
{
  "schema": 1,
  "request_id": "86d16941-55fc-4b4e-af54-5f52ef14751e",
  "node_id": "S5RPTAIORS",
  "expires_at_unix_s": 1793709030,
  "operation": "fan.set",
  "arguments": { "action": "on" }
}
```

The node accepts expiration from its current Unix time through at most
300 seconds in the future. Keep the Pi and server clocks synchronized.
`issued_at_unix_s` is an optional numeric field; expiration is checked against
the Pi's clock. Other unexpected envelope fields are rejected. `request_id`
must be nonempty, at most 128 characters, and contain only letters, digits,
hyphen, underscore, period, or colon. The node ID must match the normalized
uppercase ID exactly. Retained command deliveries are ignored.

Supported operations map to fixed `aiorsctl` argument vectors:

| Operation | Arguments |
|---|---|
| `route.set` | `channel` (`a`/`b`), `port` (`ai`/`bi`/`ae`/`be`), `side` (`rx`/`tx`) |
| `route.set_all` | `a_rx_port`, `a_tx_port`, `b_rx_port`, `b_tx_port` |
| `digital_output.set` | `channel` (`0`-`5`), `action` (`on`/`off`/`toggle`) |
| `fan.set` | `action` (`on`/`off`/`toggle`) |
| `usb_power.set` | `port` (`2`-`5`), `action` (`on`/`off`) |
| `led.set` | `channel` (`a`/`b`), `side` (`rx`/`tx`), `action` (`on`/`off`/`toggle`) |
| `watchdog.pulse` | Empty object |

Debug and stress commands are deliberately unavailable. The agent never runs
a shell. It validates every operation and value, calls `aiorsctl` with a fixed
argument array, enforces `TIMEOUT_MS`, limits captured output, rejects expired
or wrong-node requests, and stores completed request IDs in SQLite. A repeated
`request_id` returns the stored response without executing the hardware command
again while its record is retained and the request passes envelope, node-ID,
and expiration validation. An expired duplicate receives an expiration error
before the cached result is considered. The command payload limit is 64 KiB;
the captured output buffer is 8192 bytes, with longer output truncated.

The request ID is durably claimed before `aiorsctl` starts. If power is lost in
the narrow interval between starting the hardware action and saving its reply,
the same request is not executed again; its response reports
`previous_execution_outcome_unknown` and the server can require an operator to
inspect current state.

Duplicate detection survives agent and Pi restarts through SQLite. It is
bounded by retention: records older than 30 days are pruned at startup and
hourly. Reusing an ID after its record has been pruned can cause a new
execution. Servers should keep their own request ledger and never reuse IDs.

Completed command responses use the common envelope and include `request_id`,
`operation`, `ok`, `exit_code`, `timed_out`, and `output`. For example:

```json
{
  "schema": 1,
  "kind": "command_response",
  "node_id": "S5RPTAIORS",
  "topic_node_id": "S5RPTAIORS",
  "boot_id": "42f81f04-66e6-4692-8dde-81a8d8f7e19b",
  "sequence": 1844,
  "published_at": "2026-11-03T12:32:00Z",
  "request_id": "86d16941-55fc-4b4e-af54-5f52ef14751e",
  "operation": "fan.set",
  "ok": true,
  "exit_code": 0,
  "timed_out": false,
  "output": "fan=on\n"
}
```

Rejected requests instead contain `ok=false` and an `error` string, such as
`node_id_mismatch`, `expired_or_too_far_in_future`, `operation_not_allowed`,
or `previous_execution_outcome_unknown`. Invalid or unavailable request IDs
can be reported as `unknown`. Include the command-response topic in the
server subscription if remote control is enabled.

Enable commands only after broker ACLs and the server authorization path have
been tested:

```ini
[commands]
ENABLED=1
TIMEOUT_MS=3000
```

The FRN server should authenticate its human/API caller, authorize the exact
node and operation, generate a globally unique `request_id`, use a short
expiration (for example 30 seconds), publish non-retained at QoS 1, and wait
for the matching response. It must never retry with a new request ID unless a
second hardware action is intended.

## Failure Tests

Run these before depending on telemetry operationally:

1. Stop the broker. Confirm `aiorsd` and `svxlink` remain active and the SQLite
   outbox stays within `MAX_QUEUE_MESSAGES` and `MAX_QUEUE_BYTES`.
2. Restart the broker. Confirm queued messages drain and duplicates are ignored
   by the server idempotency key.
3. Stop `svxlink`. Confirm the MQTT agent remains alive and the last retained
   telemetry stays available; the server should mark it stale by timestamp.
4. Stop the MQTT agent normally while connected. Confirm it publishes retained
   `offline` with reason `service_stopped`; this tests graceful shutdown.
5. In a separate controlled test, terminate the agent unexpectedly, for
   example with SIGKILL. Confirm the broker publishes its Last Will as
   `offline` with reason `connection_lost`. The service is configured to
   restart after failure, so record the transition even if `online` follows.
6. Publish a retained, expired, wrong-node, unknown-operation, and duplicate-ID
   command. Confirm none causes an unintended repeated hardware action.
7. Compare `aiorsctl get radio status all` with the four physical MQTT fields.
   Confirm disabled and unimplemented ports do not add new radio faults, while
   a controlled managed-radio failure changes the appropriate state/counters.
8. Restore services, connections, and radio settings after testing. Confirm
   fresh snapshots and new messages arrive at the configured intervals.

Queue-capacity tests check outbox limits separately from actual database and
filesystem usage. Delivery tests should also exercise capacity eviction so
the server handles missing samples or responses without assuming lossless
storage. Run hardware-failure tests only in a controlled maintenance session.

## Implementation References

The implementation behind this guide is in these source files:

- AIORS: `src/mqtt_agent.c`, `src/mqtt_agent_core.c`, and `src/cli.c`.
- AIORS deployment defaults: `etc/aiors-mqtt/agent.conf`,
  `etc/systemd/system/aiors-mqtt-agent.service`, `deploy/svxlink.conf`, and
  `scripts/install_aiors_svxlink.sh`.
- SvxLink: `src/svxlink/svxlink/SvxStats.cpp` and `SvxStats.h`.
- Deployment package: `install.sh`, `scripts/package-release.sh`, and the
  Mosquitto examples alongside this guide.

The Markdown guide is the source for `MQTT_TELEMETRY_AND_SERVER.docx`.
Regenerate the Word copy whenever the Markdown guide changes.
