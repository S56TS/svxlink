# AIORS + SvxLink MQTT Telemetry

Architecture reviewed against local source on 28 September 2026; software
identity reporting updated on 20 October 2026. MQTT envelope schema: 1;
SvxLink snapshot schema: 3; AIORS diagnostics schema: 4. This edition updates
the existing aiors_deploy guide. The architecture review covers source,
configuration, installer, service and broker examples; it does not certify a
running Pi, broker, or the locally modified prebuilt binaries.

## Architecture

`aiors-mqtt-agent` is a third, independent service. It is not linked into
`svxlink` or `aiorsd` and neither radio service requires it.

The agent:

1. Reads the atomic SvxLink snapshot at
   `/var/lib/svxlink/svxstats_snapshot.txt`.
2. Converts every `key=value` field into a JSON telemetry object.
3. Stores outbound telemetry, events, and command responses in
   `/var/lib/aiors-mqtt/queue.db` before publishing.
4. Publishes with MQTT QoS 1 and normally removes delivered items after PUBACK; capacity eviction can remove undelivered items.
5. Reconnects with bounded backoff when the broker or network is unavailable.

The queue is bounded by both `MAX_QUEUE_MESSAGES` (default 10080) and
`MAX_QUEUE_BYTES` (default 50 MiB). When full, the oldest low-priority
telemetry is discarded before events or command responses. Priority is 10 for telemetry, 50 for events, and 100 for responses. Within a
priority, older entries are evicted first. Events and responses can also be
discarded if the limits require it. Publishing selects higher priority first,
then oldest first, with one queued publish in flight.

These limits bound outbox rows and the SQLite sum of topic/payload text lengths.
They do not cap the database file, WAL, indexes, or command history. Completed
command records have no automatic pruning. Monitor actual filesystem usage.
The agent has no code path that stops or restarts either radio service.

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
`%23`. The normalized uppercase ID remains unencoded in each JSON payload as `node_id`.
An explicit `NODE_ID` is also uppercased. Even with an override, the SvxLink
configuration must be readable and contain `[GLOBAL] LOGICS`; `logic` is its first entry.

## Topics

For `CALLSIGN=S5RPTAIORS`, the agent uses:

| Topic | QoS | Retained | Contents |
|---|---:|---:|---|
| `frn/aiors/v1/S5RPTAIORS/status` | 1 | yes | Online/offline state, agent Git identity, boot ID, command state, queue depth |
| `frn/aiors/v1/S5RPTAIORS/telemetry` | 1 | yes | Complete latest SvxLink statistics snapshot, including AIORS diagnostics |
| `frn/aiors/v1/S5RPTAIORS/events` | 1 | no | Important AIORS state and fault transitions |
| `frn/aiors/v1/S5RPTAIORS/command/request` | 1 | no | Server-to-node structured control request; subscription is disabled by default |
| `frn/aiors/v1/S5RPTAIORS/command/response` | 1 | no | Durable result for a command request |

The broker Last Will sets retained status to `offline` after an unexpected
disconnect. A graceful service stop publishes `offline` with reason
`service_stopped`.

## Telemetry Payload

Example envelope:

```json
{
  "schema": 1,
  "kind": "telemetry",
  "node_id": "S5RPTAIORS",
  "topic_node_id": "S5RPTAIORS",
  "boot_id": "42f81f04-66e6-4692-8dde-81a8d8f7e19b",
  "sequence": 1842,
  "published_at": "2026-08-24T12:30:00Z",
  "logic": "RepeaterLogic",
  "source": "/var/lib/svxlink/svxstats_snapshot.txt",
  "metrics": {
    "stats_schema": 3,
    "ts_unix_s": 1787574600,
    "aiors_diagnostics_version": "5",
    "aiors_software_version": "4f165e5-dirty",
    "aiors_software_revision": "4f165e5-dirty",
    "aiors_hardware_version": "1.0",
    "aiors_overall_status": "ok",
    "aiors_psu_a_input_voltage_v": 13.72,
    "aiors_psu_a_energy_wh_total": 126.31
  }
}
```

`na` is transmitted as JSON `null`. Numeric snapshot values become JSON
numbers. Keys containing `status`, `state`, `version`, `revision`, `scope`, `error`,
`reason`, or `boot_id` remain strings, even when the value looks numeric.
Other nonnumeric values also remain strings. This means, for example,
`aiors_psu_a_state_change_evt_total` is a string. Consumers must follow the
actual JSON type rather than assume all counters are numbers.

## Software and hardware identity

AIORS uses the Git description embedded at build time as its software version;
there is no separately maintained software release number. A description can
be a short commit hash, a tag-based description, or either with `-dirty` for
uncommitted build changes. `unknown` means Git metadata was unavailable and no
`GIT_VERSION` override was supplied. A dirty build cannot be reproduced from
the named commit alone; use committed source for release builds.

The existing field names are retained:

| Location | Fields | Value |
|---|---|---|
| MQTT `status` | `agent_version`, `agent_revision` | Both contain the MQTT agent's Git description |
| MQTT `telemetry.metrics` | `aiors_software_version`, `aiors_software_revision` | Both contain the running AIORS daemon's Git description, sampled by SvxLink |
| MQTT `telemetry.metrics` | `aiors_hardware_version` | Board revision, `1.0` or `1.1` |
| MQTT `telemetry.metrics` | `svxlink_software_version`, `svxlink_software_revision` | SvxLink's own version and revision; their existing formats are unchanged |

For example, these fields appear in a status message (excerpt):

```json
{
  "schema": 1,
  "kind": "status",
  "node_id": "S5RPTAIORS",
  "state": "online",
  "agent_version": "4f165e5-dirty",
  "agent_revision": "4f165e5-dirty"
}
```

Store software identifiers and hardware revisions as strings, including hashes
containing only digits. Do not parse them as dotted release numbers or sort
them numerically to determine which build is newer. The agent and daemon are
separate binaries and may report different revisions after partial upgrades.
Daemon identity in telemetry follows SvxLink's sampling interval; unavailable
identity fields are `null` through the normal `na` conversion.

Check the binaries installed on a node with:

```sh
aiorsd --version
aiorsctl --version
aiorsctl version
aiors-mqtt-agent --version
```

Example output when all AIORS components were built from the same modified
source for hardware 1.0:

```text
aiorsd 4f165e5-dirty (hw 1.0)
aiorsctl 4f165e5-dirty (hw 1.0)
aiorsd 4f165e5-dirty (hw 1.0)
aiors-mqtt-agent 4f165e5-dirty
```

`aiorsctl version` queries the running daemon; `--version` identifies the
binary invoked. The MQTT agent is independent of the hardware revision.
FRN `get version` forwards the daemon identity alongside SvxLink's own version.

Protocol schemas remain separate from software identity: MQTT `schema` stays
numeric `1`, the topic prefix stays `frn/aiors/v1`, snapshot `stats_schema`
stays numeric `3`. AIORS diagnostics version `5` adds four physical radio
states; version `4` remains supported. `aiors_diagnostics_version` is serialized
as a string because its key contains `version`. No topic or envelope schema
migration is needed.

### Four physical radio ports

Telemetry includes these string metrics, independent of audio/PTT/COS routing:

| Metric | Physical port |
|---|---|
| `aiors_radio_ai_status` | Internal A |
| `aiors_radio_bi_status` | Internal B |
| `aiors_radio_ae_status` | External A |
| `aiors_radio_be_status` | External B |

States are `ready`, `disabled`, `unimplemented`, `absent`, `mismatch`,
`config_error`, or `uninitialized`. Each state change produces the existing
`metric_changed` event with `previous` and `current` values. Repeated unchanged
samples produce no duplicate event. Initial observations establish a baseline.

Each port also has numeric `aiors_radio_<port>_fault_evt_<window>`,
`aiors_radio_<port>_fault_samples_<window>`, and
`aiors_radio_<port>_fault_s_<window>` metrics, where `<window>` is `1h`, `24h`,
or `total`. `disabled` and `unimplemented` do not accumulate faults. Other
non-ready states count as faults. External placeholders therefore remain
visible without creating false failures.

The legacy `aiors_sa818_a_status` / `aiors_sa818_b_status` metrics and counters
remain available. With diagnostics version 5, they follow physical AI/BI and
their status becomes JSON `null` when the radio is disabled or unimplemented.
With version 4, they retain the older A/B semantics; all four explicit physical
status metrics are JSON `null` because physical identity is unavailable.
Existing historical counters are retained, while new physical-port totals
start separately. State values in change events are strings, including `na`
for unknown values; telemetry represents `na` as JSON `null`.

Use `aiors_sample_ok`, sample age, and the existing freshness fields to judge
whether a reported state is current. A failed sample keeps the last successful
radio state, rather than claiming a new hardware state.

## Snapshot sampling and fields

The agent polls every 1000 ms by default, but only queues changed snapshot
contents. SvxLink writes snapshots on `STATS_INTERVAL` (default 60 seconds);
AIORS sampling defaults to 30 seconds. Polling faster does not create new
measurements. A persisted content hash suppresses unchanged snapshots across
agent restarts. Reconnecting does not force a fresh telemetry publication.
Snapshots are limited to 1 MiB, 512 metrics, 127 characters per key and 255 per
value. Invalid snapshots are skipped.

Every field in the snapshot is sent. The current snapshot groups are:

| Group | Data |
|---|---|
| Identity and uptime | Snapshot schema/time, Linux boot ID and uptime, SvxLink runtime/version/revision, AIORS runtime/version/revision/hardware version |
| FRN link | Link up/down ages and 1-hour/lifetime counters, users, joins/leaves, RX/TX age, duration, bytes and events, plus TX duty |
| RF | RX/TX event counts, durations, min/max duration, duty, and lifetime hours |
| RunCmd | Accepted, rejected, authentication failure, broadcast attempt, average latency, and maximum latency values |
| AIORS health | Sample status/age/error, overall status, ADC/environment/digital-input status, PSU GPIO status, fan state |
| PSU A and B | Current state/fault, input voltage/current/calculated power, 1-hour and 24-hour min/max/average/sample count |
| Power totals | Per-channel Wh and Ah, measurement coverage, on/off time, fault time/events, state changes, and restart/degraded/fault totals |
| Board measurements | 3.3 V and 5 V buses, temperature, humidity, RF A/B average voltage, AI0-AI3; each includes current and 1-hour/24-hour min/max/average/sample count |
| Radio peripherals | Current SA818 A/B and CM108 A/B status plus 1-hour, 24-hour, and lifetime fault event/sample/duration counters |

The authoritative field list is the `metrics` object. Consumers should ignore
unknown fields so a future `stats_schema` can add diagnostics without breaking
ingestion.

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
  "published_at": "2026-08-24T12:31:00Z",
  "event": "metric_changed",
  "metric": "aiors_psu_a_fault",
  "previous": "no",
  "current": "yes"
}
```

The first observed value seeds persisted local state and is not reported as a
change. Event `previous` and `current` values are always strings. Events report
changes seen between snapshots; they are not a record of every hardware transition.

## Pi Configuration

Start from the managed template:

```sh
sudo cp /usr/local/share/aiors/aiors-mqtt-agent.conf \
  /etc/aiors-mqtt/agent.conf
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
sudo -u aiors-mqtt aiors-mqtt-agent --check-config \
  -c /etc/aiors-mqtt/agent.conf
sudo systemctl enable --now aiors-mqtt-agent
systemctl status aiors-mqtt-agent --no-pager
journalctl -u aiors-mqtt-agent -b --no-pager -n 100
```

For a fresh combined deployment, private files can be supplied without adding
them to Git:

```sh
MQTT_CONFIG_PATH=$HOME/private/agent.conf \
MQTT_CA_PATH=$HOME/private/ca.crt \
MQTT_PASSWORD_PATH=$HOME/private/password \
AIORS_HW_VERSION=1.1 \
DEPLOY_VERSION=deploy-v1.1.0 \
  bash install.sh
```

Existing MQTT config and credentials are preserved on upgrades when these
variables are omitted.

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

The server database should use `(node_id, boot_id, sequence)` as an idempotency
key. QoS 1 is at least once, so duplicate delivery is valid. Use this key for telemetry, events and command responses; status has no
`sequence`. Store `metrics.ts_unix_s` as snapshot time and
`metrics.aiors_sample_ts_unix_s` as AIORS measurement time. `published_at` is
generated when the message is constructed, before queueing, and is not the
actual transmission time. Record a separate server receive timestamp. Alert on retained `status.state=offline`, stale telemetry, an
increasing failure/fault counter, or threshold violations. Do not calculate a
new event by summing repeated retained snapshots; store counter samples and
derive deltas with reset detection, using uptime and the documented counter
scope. Some totals persist across reboots; other counters can reset on a
SvxLink restart within the same Linux boot.

Status is published on connection and graceful shutdown, not periodically.
Its queue counts are point-in-time values. The Last Will payload is constructed
at agent initialization, so its timestamp and counts do not describe the time
of connection loss. Reconnect backoff is bounded at 60 seconds.

## Remote Commands

Remote commands are compiled and installed but `[commands] ENABLED=0` is the
default. When disabled, the agent does not subscribe to `command/request`.

Request example:

```json
{
  "schema": 1,
  "request_id": "86d16941-55fc-4b4e-af54-5f52ef14751e",
  "node_id": "S5RPTAIORS",
  "expires_at_unix_s": 1787574900,
  "operation": "fan.set",
  "arguments": { "action": "on" }
}
```

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
again, provided envelope, node ID and expiration validation still pass.
Expired duplicates are rejected before the stored response is checked.

### Request validation and responses

- Required fields are exactly those in the example. Optional `issued_at_unix_s`
  must be numeric but is otherwise unused. Unknown or duplicate fields are rejected.
- `request_id` is 1-128 characters using letters, digits, hyphen, underscore,
  dot or colon. The node ID must match the normalized ID exactly.
- Expiration must be between current node time and 300 seconds into the future.
  Replace the historical example timestamp with a current value when testing.
- All operation arguments are JSON strings, including output channel and USB
  port numbers. Each operation requires exactly its listed arguments.
- Payloads above 64 KiB or with zero length are discarded without a response.
  Messages delivered with the MQTT retain flag are ignored without a response.
  Always publish commands non-retained; the receiver's retain check alone is
  not a guarantee against every command originally published with retention.
- Executed responses use the common envelope with `kind=command_response`,
  plus `request_id`, `operation`, `ok`, `exit_code`, `timed_out`, and `output`.
  Captured combined stdout/stderr is limited to 8191 bytes.
- Rejected responses include `ok=false` and `error`. Reasons include
  `invalid_json`, `invalid_envelope`, `node_id_mismatch`,
  `expired_or_too_far_in_future`, `operation_not_allowed`,
  `cannot_claim_request`, and `previous_execution_outcome_unknown`.

The request ID is inserted into SQLite before `aiorsctl` starts. If that
claim survives a crash but no response was saved, a valid retry reports
`previous_execution_outcome_unknown` without re-executing. Inspect hardware
state before requesting another action. The code uses SQLite WAL with
`synchronous=NORMAL`; the documentation cannot promise that the most recent
claim survives sudden power loss. Exactly-once hardware execution is not
guaranteed. Deduplication also depends on retaining the same state database.

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
   outbox row count and topic/payload length sum stay within the configured limits.
   Measure database and WAL disk usage separately.
2. Restart the broker. Confirm queued messages drain and duplicates are ignored
   by the server idempotency key.
3. Stop `svxlink`. Confirm the MQTT agent remains alive and the last retained
   telemetry stays available; the server should mark it stale by timestamp.
4. Stop the MQTT agent normally. Confirm retained status becomes `offline`
   with reason `service_stopped` when connected. Separately simulate an abrupt
   disconnect and observe Last Will reason `connection_lost`; allow for broker
   detection delay and automatic service restart. Both radio services should
   continue normally.
5. Publish a retained, expired, wrong-node, unknown-operation, and duplicate-ID
   command. Confirm none causes an unintended repeated hardware action.

## Configuration reference

Settings and section names are case-insensitive; unknown settings fail loading.
Boolean values accept 1/0, yes/no, true/false, and on/off. Defaults below are
compiled defaults; the installed template sets `CA_FILE=/etc/aiors-mqtt/ca.crt`.
The installed template
disables the agent and commands by default. `--check-config` validates local
configuration and paths; it does not connect to the broker or test its ACLs.

| Section / key | Default | Accepted range or meaning |
|---|---|---|
| agent / ENABLED | 0 | Enable agent |
| agent / NODE_ID | empty | Use selected logic CALLSIGN |
| agent / SVXLINK_CONFIG | /etc/svxlink/svxlink.conf | Identity source |
| agent / SNAPSHOT_PATH | /var/lib/svxlink/svxstats_snapshot.txt | Snapshot source |
| agent / STATE_DB | /var/lib/aiors-mqtt/queue.db | Outbox, sequence, event state, command history |
| agent / AIORSCTL_PATH | /usr/local/bin/aiorsctl | Command executable |
| agent / POLL_INTERVAL_MS | 1000 | 250-60000 ms |
| agent / MAX_QUEUE_MESSAGES | 10080 | 100-1000000 |
| agent / MAX_QUEUE_BYTES | 52428800 | 4194304-1073741824; logical text limit |
| broker / HOST | empty | Required when enabled |
| broker / PORT | 8883 | 1-65535 |
| broker / TOPIC_PREFIX | frn/aiors/v1 | No + or #; trailing slashes removed |
| broker / KEEPALIVE_S | 30 | 10-3600 seconds |
| broker / USERNAME | empty | Broker account |
| broker / PASSWORD_FILE | empty | Password read from first line |
| broker / TLS | 1 | CA_FILE required when TLS enabled |
| broker / CA_FILE | empty | Trust CA file |
| broker / CERT_FILE, KEY_FILE | empty | Client certificate/key; configure together |
| commands / ENABLED | 0 | Subscribe to command/request |
| commands / TIMEOUT_MS | 3000 | 100-30000 ms |

The service runs as `aiors-mqtt`, with supplementary `aiors` and `svxlink`
groups, a 64 MiB memory limit, and restart-on-failure after 5 seconds.
The installer must provide access to the snapshot, credentials and AIORS
control socket. MQTT status reports agent connectivity, not radio health.

## Review provenance

The software identity update on 20 October 2026 used local AIORS CLI/MQTT and
SvxLink statistics/FRN source, including the uncommitted removal of AIORS's
manual software version. CLI diagnostics, SvxLink snapshot parsing, MQTT
conversion and FRN reply formatting were checked with ARM64 test programs.
This did not include a live broker or FRN connection. Repository HEADs for
that update were AIORS BSP `4f165e54fe33814c3510f35fe6f77c5483454a1a` and
SvxLink `d3c4f4fa6ad0652bcc599c7e91556f3eb0d8dd96`; HEAD alone does not identify
the uncommitted changes.

The original 28 September architecture review used local working files,
including uncommitted files where present. It did not fetch remote branches.
Repository HEADs at that earlier review:

- SvxLink: 9c24ac6e1c513234d8eea9bfb4738cb8d0dde3bb
- AIORS BSP: 18826790dd585b032e0e74ffe23324d5c0affaf0
- AIORS deploy: 6449fddcfa220dbe698bffebc0cfb32b90fbd73e

The original guide is in `aiors_bsp/.git/aiors_deploy/docs` in this checkout
layout. This reviewed edition and its DOCX are in `svxlink/docs`.
SHA-256 fingerprints below identify the implementation files from that earlier
architecture review, not the October software identity update.

svxlink/src/svxlink/svxlink/SvxStats.cpp

```text
cc2a4a3f7000497d446ed88e9576063e79aa996655b6b082b03e3aca81053ad4
```

svxlink/src/svxlink/svxlink/svxlink.cpp

```text
e9a31b33b7c3dd7e35cf8c3595e55a83ebe1bf8e405b088f28b35fe00f81f643
```

aiors_bsp/src/mqtt_agent.c

```text
0f8c9cde4b13ac688cd519dc0e64350254cbeaf4779d0e27f369cc3ec6431c7e
```

aiors_bsp/src/mqtt_agent_core.c

```text
6fa9ddfa62e9f234ce39e46a321904de17e3e32b1f1a25491f27f5cfbf30e293
```

aiors_bsp/include/mqtt_agent_core.h

```text
faeb4d8f444d2d04bb45f35b18004264203d6f8118b816b6c25408710df2b4df
```

aiors_bsp/etc/aiors-mqtt/agent.conf

```text
ca79cc236552e9e66ff75f165531ef62aae4dcb28638e4af0b50ce0412fb99c0
```

aiors_bsp/etc/systemd/system/aiors-mqtt-agent.service

```text
3e44552aa22d7e70cd62ab5c5344bb79fb11d72051380a42c4f6adde357f8133
```

aiors_bsp/scripts/install_aiors_svxlink.sh

```text
e9914f560bbe3fa54ca5bb6a41a20b1b30d5df459c6dd7b3d72218f2df6c4ac1
```

aiors_deploy/install.sh

```text
4c295c44a8dc502f82b9df460034f48f4499a76a62ee22c3643e8eafdf75fde6
```

aiors_deploy/docs/mosquitto-aiors.conf.example

```text
f79f719cc866bf23806ddfc2b73e62de4279dff8f9dcef073557596dde61a618
```

aiors_deploy/docs/mosquitto-aiors.acl.example

```text
dc9bb2d640e8220ad743eb3ae3b6bb83f8b5c854e2941911aa14cf3031d8dfbe
```
