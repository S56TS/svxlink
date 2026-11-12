// Included by test_svxstats_radios.py after the production implementation.
#include <cassert>

static std::string change_field(std::string text, const std::string& key,
                                const std::string& value)
{
  const size_t start = text.find(key + "=");
  assert(start != std::string::npos);
  const size_t end = text.find('\n', start);
  text.replace(start, end - start, key + "=" + value);
  return text;
}

static std::string remove_field(std::string text, const std::string& key)
{
  const size_t start = text.find(key + "=");
  assert(start != std::string::npos);
  text.erase(start, text.find('\n', start) + 1 - start);
  return text;
}

static SvxStats::AiorsSample parse(SvxStats& stats, const std::string& text)
{
  SvxStats::AiorsSample sample;
  std::string error;
  assert(stats.parseAiorsDiagnostics(text, sample, error));
  return sample;
}

int main(int argc, char** argv)
{
  assert(argc >= 3);
  std::ifstream input(argv[1]);
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  assert(!text.empty());
  SvxStats stats;
  assert(stats.registerLogic("RepeaterLogic", "Repeater", "327CRMCE0", "Rx1", "Tx2"));
  assert(stats.registerLogic("SimplexLogic", "Simplex", "327CRMCE0-2", "Rx2", "Tx1"));
  stats.setLogicState("RepeaterLogic", "ready");
  stats.setLogicState("SimplexLogic", "ready");
  const auto initial = parse(stats, text);
  assert(initial.diagnostics_version == 5);

  for (size_t radio = 0; radio < SvxStats::AIORS_RADIO_COUNT; ++radio)
  {
    const std::string key = std::string("radio_") + SvxStats::aiors_radio_names[radio] + "_status";
    for (const std::string state : {"ready", "disabled", "unimplemented", "absent", "mismatch", "config_error", "uninitialized"})
    {
      const auto sample = parse(stats, change_field(text, key, state));
      const std::string expected = state == "ready" ? "ok" :
          (state == "disabled" || state == "unimplemented" ? "na" : "fault");
      assert(sample.radio_states[radio] == state);
      assert(sample.statuses[SvxStats::AIORS_STATUS_RADIO_AI + radio] == expected);
      if (radio < 2) assert(sample.statuses[radio] == expected);
    }
    SvxStats::AiorsSample rejected;
    std::string error;
    assert(!stats.parseAiorsDiagnostics(remove_field(text, key), rejected, error));
    assert(error == "missing_" + key);
    assert(!stats.parseAiorsDiagnostics(change_field(text, key, "broken"), rejected, error));
    assert(error == "invalid_" + key);
  }

  std::string legacy = change_field(text, "diagnostics_version", "4");
  for (const auto* name : SvxStats::aiors_radio_names)
    legacy = remove_field(legacy, std::string("radio_") + name + "_status");
  legacy = change_field(legacy, "sa818_a_status", "fault");
  const auto old = parse(stats, legacy);
  assert(old.statuses[SvxStats::AIORS_STATUS_SA818_A] == "fault");
  for (size_t radio = 0; radio < SvxStats::AIORS_RADIO_COUNT; ++radio)
  {
    assert(old.radio_states[radio] == "na");
    assert(old.statuses[SvxStats::AIORS_STATUS_RADIO_AI + radio] == "na");
  }

  stats.aiors_diagnostics_enabled = true;
  auto add = [&](const std::string& payload, double time)
  {
    auto sample = parse(stats, payload);
    sample.monotonic_s = time;
    sample.uptime_s = static_cast<uint64_t>(time + 1000);
    stats.updateAiorsTotals(sample);
    stats.aiors_samples.push_back(sample);
  };
  const std::string disabled = change_field(change_field(text, "radio_ai_status", "disabled"), "sa818_a_status", "fault");
  add(disabled, 100);
  add(change_field(disabled, "radio_ae_status", "absent"), 130);
  add(change_field(disabled, "radio_ae_status", "ready"), 160);
  assert(stats.aiors_totals.statuses[SvxStats::AIORS_STATUS_SA818_A].fault_samples == 0);
  assert(stats.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AI].fault_samples == 0);
  assert(stats.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_BE].fault_samples == 0);
  const auto& external = stats.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AE];
  assert(external.fault_events == 1 && external.fault_samples == 1 && external.fault_s == 30);
  for (const double window : {3600.0, 86400.0})
  {
    const auto aggregate = stats.aggregateAiorsWindow(170, window);
    assert(aggregate.statuses[SvxStats::AIORS_STATUS_RADIO_AE].fault_s == 30);
    assert(aggregate.statuses[SvxStats::AIORS_STATUS_RADIO_AE].fault_events == 1);
    assert(aggregate.statuses[SvxStats::AIORS_STATUS_SA818_A].fault_samples == 0);
  }

  stats.last_aiors_attempt_ok = true;
  stats.last_aiors_success_monotonic = mono_now_sec();
  const auto groups = stats.formatStatsGroups();
  assert(!groups.empty());
  assert(groups.front().find("DIAGNOSTICS scope=node") == 0);
  std::string all_groups;
  for(const auto& group: groups) all_groups += group;
  assert(all_groups.find("aiors_radio_ai_status=disabled") != std::string::npos);
  assert(all_groups.find("scope=logic logic=RepeaterLogic") != std::string::npos);
  assert(all_groups.find("scope=logic logic=SimplexLogic") != std::string::npos);
  assert(stats.formatDiagnosticGroups("logic", "missing").empty());
  const auto own = stats.formatDiagnosticGroups("logic", "SimplexLogic");
  assert(!own.empty());
  for(const auto& group : own) {
    assert(group.find("DIAGNOSTICS scope=logic logic=SimplexLogic") == 0);
    assert(group.find("aiors_") == std::string::npos);
  }

  // Independent activity flags: stopping one logic must leave the other active.
  stats.onRfTxState("RepeaterLogic", true);
  stats.onRfTxState("SimplexLogic", true);
  stats.logic_states.at("RepeaterLogic").rf_tx_t0 = mono_now_sec()-10;
  stats.logic_states.at("SimplexLogic").rf_tx_t0 = mono_now_sec()-20;
  stats.onRfTxState("RepeaterLogic", false);
  assert(stats.logic_states.at("SimplexLogic").rf_tx_active);
  stats.onRfTxState("SimplexLogic", false);
  assert(stats.logic_states.at("RepeaterLogic").rf_tx_total.evt == 1);
  assert(stats.logic_states.at("RepeaterLogic").rf_tx_total.sec >= 9.5);
  assert(stats.logic_states.at("SimplexLogic").rf_tx_total.sec >= 19.5);
  stats.onRfRxState("RepeaterLogic", true);
  stats.onRfRxState("SimplexLogic", true);
  stats.onRfRxState("RepeaterLogic", false);
  assert(stats.logic_states.at("SimplexLogic").rf_rx_active);
  stats.onRfRxState("SimplexLogic", false);
  stats.onSquelchState("RepeaterLogic", true);
  stats.onSquelchState("SimplexLogic", true);
  stats.onSquelchState("RepeaterLogic", false);
  assert(stats.logic_states.at("SimplexLogic").sql_active);
  stats.onSquelchState("SimplexLogic", false);

  stats.onFrnClientListUpdate("RepeaterLogic", {"A", "B"});
  stats.onFrnClientListUpdate("SimplexLogic", {"A"});
  stats.onFrnClientListUpdate("RepeaterLogic", {"A", "B", "C"});
  const auto& first = stats.logic_states.at("RepeaterLogic");
  const auto& second = stats.logic_states.at("SimplexLogic");
  assert(first.frn_users_cur == 3 && second.frn_users_cur == 1);
  assert(first.buckets[first.idx].user_join == 3 && first.buckets[first.idx].user_leave == 0);
  assert(second.buckets[second.idx].user_join == 1 && second.buckets[second.idx].user_leave == 0);
  stats.onFrnTxState("RepeaterLogic", true);
  stats.onFrnTxState("SimplexLogic", true);
  stats.onFrnTxState("RepeaterLogic", false);
  assert(second.frn_tx_active);
  stats.onFrnTxState("SimplexLogic", false);
  stats.onFrnRxState("RepeaterLogic", true);
  stats.onFrnRxState("SimplexLogic", true);
  stats.onFrnRxState("SimplexLogic", false);
  assert(first.frn_rx_active);
  stats.onFrnRxState("RepeaterLogic", false);
  stats.addFrnTxBytes("RepeaterLogic", 100);
  stats.addFrnTxBytes("SimplexLogic", 500);
  stats.onCmdAccepted("RepeaterLogic");
  stats.onCmdRejected("SimplexLogic");
  assert(first.frn_tx_bytes_total == 100 && second.frn_tx_bytes_total == 500);
  assert(first.cmd_total.ok == 1 && first.cmd_total.rej == 0);
  assert(second.cmd_total.ok == 0 && second.cmd_total.rej == 1);
  assert(!stats.registerLogic("Third", "Repeater", "X", "Rx1", "Tx1"));
  assert(!stats.registerLogic("RepeaterLogic", "Repeater", "X", "Rx1", "Tx1"));
  assert(stats.setPrimaryLogic("SimplexLogic"));

  const auto snapshot = stats.formatStatsLine();
  assert(snapshot.find(" aiors_radio_ai_status=disabled") != std::string::npos);
  assert(snapshot.find(" aiors_radio_ae_status=ready") != std::string::npos);
  assert(snapshot.find(" aiors_sa818_a_status=na") != std::string::npos);
  assert(snapshot.find(" aiors_radio_ae_fault_evt_total=1") != std::string::npos);
  std::istringstream tokens(snapshot);
  std::set<std::string> keys;
  std::string token;
  tokens >> token;
  assert(token == "STATS");
  while (tokens >> token)
    assert(keys.insert(token.substr(0, token.find('='))).second);
  assert(keys.size() <= 1024);
  assert(snapshot.find(" logic_count=2") != std::string::npos);
  assert(snapshot.find(" primary_logic=SimplexLogic") != std::string::npos);
  assert(snapshot.find(" logic0_frn_tx_bytes_total=100") != std::string::npos);
  assert(snapshot.find(" logic1_frn_tx_bytes_total=500") != std::string::npos);
  assert(snapshot.find(" frn_tx_bytes_total=500") != std::string::npos);
  assert(snapshot.find("logic0_aiors_") == std::string::npos);
  assert(snapshot.find("logic1_aiors_") == std::string::npos);

  stats.persist_path = argv[2];
  stats.savePersistedTotals();
  std::ofstream(argv[2]) << stats.stats_persist_text;
  SvxStats restored;
  assert(restored.registerLogic("SimplexLogic", "Simplex", "Y", "Rx2", "Tx1"));
  assert(restored.registerLogic("RepeaterLogic", "Repeater", "X", "Rx1", "Tx2"));
  restored.persist_path = argv[2];
  restored.loadPersistedTotals();
  assert(restored.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AE].fault_events == 1);
  assert(restored.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AE].fault_s == 30);
  assert(restored.logic_states.at("RepeaterLogic").frn_tx_bytes_total == 100);
  assert(restored.logic_states.at("SimplexLogic").frn_tx_bytes_total == 500);
  assert(restored.logic_states.at("RepeaterLogic").rf_tx_total.evt == 1);
  std::ofstream(argv[2]) << "rf_tx_s_total=123\nfrn_tx_bytes_total=777\naiors_sa818_a_fault_evt_total=42\naiors_cm108_b_fault_s_total=17\n";
  SvxStats old_totals;
  assert(old_totals.registerLogic("RepeaterLogic", "Repeater", "X", "Rx1", "Tx2"));
  old_totals.persist_path = argv[2];
  old_totals.loadPersistedTotals();
  assert(old_totals.aiors_totals.statuses[SvxStats::AIORS_STATUS_SA818_A].fault_events == 42);
  assert(old_totals.aiors_totals.statuses[SvxStats::AIORS_STATUS_CM108_B].fault_s == 17);
  assert(old_totals.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AI].fault_events == 0);

  assert(old_totals.legacy_totals_available);
  assert(old_totals.legacy_totals.rf_tx_total.sec == 123);
  assert(old_totals.legacy_totals.frn_tx_bytes_total == 777);
  assert(old_totals.logic_states.at("RepeaterLogic").rf_tx_total.sec == 0);
  old_totals.savePersistedTotals();
  assert(old_totals.stats_persist_text.find("legacy.frn_tx_bytes_total=777") != std::string::npos);

  SvxStats single;
  assert(single.registerLogic("LocalSimplex", "Simplex", "SINGLE", "Rx1", "Tx1"));
  single.setLogicState("LocalSimplex", "failed");
  const std::string single_snapshot = single.formatStatsLine();
  assert(single_snapshot.find(" logic_count=1") != std::string::npos);
  assert(single_snapshot.find(" logic0_frn_enabled=0") != std::string::npos);
  assert(single_snapshot.find(" logic0_state=failed") != std::string::npos);
  assert(single_snapshot.find("logic1_") == std::string::npos);
  if(argc > 4) std::ofstream(argv[4]) << single_snapshot << "\n";

  for (const auto* name : {"ai", "bi", "ae", "be", "all"})
    assert(isSupportedRunCmd(std::string("get radio status ") + name));
  assert(!isSupportedRunCmd("get stats"));
  for(const auto* command : {"get diagnostics", "get diagnostics node", "get diagnostics logic",
                             "get diagnostics logic SimplexLogic", "get diagnostics all", "get version"})
    assert(isSupportedRunCmd(command));
  assert(!isSupportedRunCmd("get diagnostics logic SimplexLogic;reboot"));
  assert(!isSupportedRunCmd("get radio status invalid"));
  assert(!isSupportedRunCmd("get radio status ai; reboot"));
  if (argc > 3) std::ofstream(argv[3]) << snapshot << "\n";
  std::cout << "Two-logic isolation, four-radio parsing, migration, persistence, FRN and " << keys.size()
            << "-metric snapshot checks passed\n";
}
