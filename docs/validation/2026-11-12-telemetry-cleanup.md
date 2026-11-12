# Scoped telemetry cleanup

SvxLink snapshot schema 5 publishes logic statistics only in its named logic
slots. The MQTT agent converts these to `logics[name]`, with shared node and
hardware data in `metrics`. The primary-logic selector, flat primary copies,
old FRN link-count aliases, and `uptime_s` alias are removed. Use
`svxlink_uptime_s` and the explicit `frn_link_*_evt_1h` counters instead.

The two-logic regression snapshot contains 473 fields, compared with 551 before
the cleanup. Historical totals remain preserved. The MQTT agent can read older
snapshots and queued messages; schema-4 flat copies are suppressed when it
constructs new telemetry. Already queued payloads retain their original shape.

Validated on 12 November 2026:

- SvxStats isolation, simultaneous activity, radio parsing, migration, persistence,
  reordering, and absence of flat logic aliases.
- The real MQTT JSON/SQLite pipeline, including schema-4 duplicate suppression,
  one/two logic blocks, scoped events, failed states, unavailable FRN fields,
  and legacy snapshots.
- MQTT core/configuration parsing and four-radio event/typing checks.
- Five installer state tests and the FRN diagnostic parser/authentication tests.
- ARM64 compilation of SvxLink and ModuleFrn.
- The real ARM64 MQTT agent with libmosquitto and a local MQTT peer: broker
  outage queueing, reconnect, retained QoS 1 telemetry, PUBACK deletion, and
  unacknowledged delivery after disconnect. The payload had 322 shared metrics.
- Both Markdown manuals regenerated and their DOCX structure validated.
- SvxLink deployment bundle and companion MQTT executable rebuilt, both checksum
  manifests verified, packaged SvxLink/FRN files compared with staging, and both
  release executables started successfully under ARM64 emulation.

The local MQTT peer does not exercise production TLS, ACLs, a live receiving
server/dashboard, or physical radio measurements. Consumers must read logic
statistics from `logics[name]` before deploying the matching updated binaries.
