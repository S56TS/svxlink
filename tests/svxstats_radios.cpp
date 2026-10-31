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
  assert(groups.size() == 7);
  assert(groups.back().find("RADIOS ai=disabled bi=ready ae=ready be=unimplemented sample_ok=1") == 0);
  assert(groups.back().size() < 200);
  stats.aiors_diagnostics_enabled = false;
  assert(stats.formatStatsGroups().size() == 6);

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
  assert(keys.size() <= 512);

  stats.persist_path = argv[2];
  stats.savePersistedTotals();
  std::ofstream(argv[2]) << stats.stats_persist_text;
  SvxStats restored;
  restored.persist_path = argv[2];
  restored.loadPersistedTotals();
  assert(restored.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AE].fault_events == 1);
  assert(restored.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AE].fault_s == 30);
  std::ofstream(argv[2]) << "aiors_sa818_a_fault_evt_total=42\naiors_cm108_b_fault_s_total=17\n";
  SvxStats old_totals;
  old_totals.persist_path = argv[2];
  old_totals.loadPersistedTotals();
  assert(old_totals.aiors_totals.statuses[SvxStats::AIORS_STATUS_SA818_A].fault_events == 42);
  assert(old_totals.aiors_totals.statuses[SvxStats::AIORS_STATUS_CM108_B].fault_s == 17);
  assert(old_totals.aiors_totals.statuses[SvxStats::AIORS_STATUS_RADIO_AI].fault_events == 0);

  for (const auto* name : {"ai", "bi", "ae", "be", "all"})
    assert(isSupportedRunCmd(std::string("get radio status ") + name));
  assert(isSupportedRunCmd("get stats") && isSupportedRunCmd("get version"));
  assert(!isSupportedRunCmd("get radio status invalid"));
  assert(!isSupportedRunCmd("get radio status ai; reboot"));
  if (argc > 3) std::ofstream(argv[3]) << snapshot << "\n";
  std::cout << "Four-radio parsing, fault histories, persistence, FRN and " << keys.size()
            << "-metric snapshot checks passed\n";
}
