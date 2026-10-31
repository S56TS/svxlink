#ifndef SVX_STATS_H
#define SVX_STATS_H

#include <string>
#include <vector>
#include <array>
#include <deque>
#include <unordered_map>
#include <cstdint>
#include <iosfwd>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace Async { class Exec; class Timer; }

class SvxStats
{
public:


  struct DurAgg
  {
    uint64_t evt = 0;
    double   sec = 0.0;
    double   min_sec = 0.0; // 0 means undefined
    double   max_sec = 0.0;
    double   sum_sec = 0.0; // for avg
  };



  struct CmdAgg
  {
    uint64_t ok = 0;
    uint64_t rej = 0;
    uint64_t authfail = 0;
    uint64_t bc_attempt = 0;
    uint64_t sum_ms = 0;
    uint64_t max_ms = 0;
    uint64_t cnt_ms = 0;
  };

  static SvxStats& instance();

  // Start periodic STATS logging (safe to call multiple times)
  void start();
  void start(uint32_t interval_s);
  void setPersistPath(const std::string& path);
  void setSnapshotPath(const std::string& path);
  void setHistoryPath(const std::string& path);
  void setLogToStdout(bool enable);
  void setAiorsDiagnosticsEnabled(bool enable);
  void setAiorsctlPath(const std::string& path);
  void setAiorsSampleInterval(uint32_t interval_s);
  void loadPersistedTotals();
  void savePersistedTotals();

  // --- FRN side ---
  void onFrnClientListUpdate(const std::vector<std::string>& client_list);
  void onFrnTxState(bool is_tx);
  void onFrnRxState(bool is_rx);
  void addFrnTxBytes(uint64_t bytes);
  void addFrnRxBytes(uint64_t bytes);
  // Link/connectivity (FRN TCP session up/down)
  void onFrnLinkUp();
  void onFrnLinkDown();

  // --- RF/repeater side ---
  void onSquelchState(bool is_open);
  void onRfTxState(bool is_tx);
  void onRfRxState(bool is_rx);

  // --- RunCmd ---
  void onCmdAccepted();
  void onCmdRejected();
  void onCmdAuthFailed();
  void onCmdBroadcastAttempt();
  void onCmdExecTimeMs(uint32_t ms);

  // Format current stats snapshot as a single-line payload (without timestamp)
  std::string formatStatsLine();
  std::vector<std::string> formatStatsGroups();

private:
  SvxStats();
  ~SvxStats();
  SvxStats(const SvxStats&) = delete;
  SvxStats& operator=(const SvxStats&) = delete;

  void onTick(Async::Timer* t);
  void onAiorsSampleTimer(Async::Timer* t);
  void rotateBuckets(uint64_t now_minute);
  uint64_t uptimeSeconds() const;
  int64_t linuxUptimeSeconds() const;
  void writeStatsReport(const std::string& line);
  void statsWriterLoop();
  void collectAiorsDiagnostics();
  void onAiorsDiagnosticsExited();
  void recordAiorsFailure(const std::string& reason);



  struct Bucket
  {
    uint64_t minute = 0;
    DurAgg frn_tx;
    DurAgg frn_rx;
    DurAgg rf_tx;
    DurAgg rf_rx;
    DurAgg sql;
    CmdAgg cmd;
    uint64_t frn_tx_bytes = 0;
    uint64_t frn_rx_bytes = 0;
    uint64_t frn_link_up = 0;
    uint64_t frn_link_down = 0;
    uint64_t user_join = 0;
    uint64_t user_leave = 0;
  };

  enum AiorsMetric
  {
    AIORS_PSU_A_VOLTAGE = 0,
    AIORS_PSU_A_CURRENT,
    AIORS_PSU_A_POWER,
    AIORS_PSU_B_VOLTAGE,
    AIORS_PSU_B_CURRENT,
    AIORS_PSU_B_POWER,
    AIORS_BUS_3V3_VOLTAGE,
    AIORS_BUS_5V_VOLTAGE,
    AIORS_RF_A_VOLTAGE,
    AIORS_RF_B_VOLTAGE,
    AIORS_TEMPERATURE,
    AIORS_HUMIDITY,
    AIORS_AI0_VOLTAGE,
    AIORS_AI1_VOLTAGE,
    AIORS_AI2_VOLTAGE,
    AIORS_AI3_VOLTAGE,
    AIORS_METRIC_COUNT
  };

  enum AiorsRadio
  {
    AIORS_RADIO_AI = 0,
    AIORS_RADIO_BI,
    AIORS_RADIO_AE,
    AIORS_RADIO_BE,
    AIORS_RADIO_COUNT
  };

  enum AiorsStatus
  {
    AIORS_STATUS_SA818_A = 0,
    AIORS_STATUS_SA818_B,
    AIORS_STATUS_CM108_A,
    AIORS_STATUS_CM108_B,
    AIORS_STATUS_RADIO_AI,
    AIORS_STATUS_RADIO_BI,
    AIORS_STATUS_RADIO_AE,
    AIORS_STATUS_RADIO_BE,
    AIORS_STATUS_COUNT
  };

  static const char* const aiors_radio_names[AIORS_RADIO_COUNT];
  static const char* const aiors_status_names[AIORS_STATUS_COUNT];

  struct AiorsSample
  {
    double monotonic_s = 0.0;
    uint64_t unix_s = 0;
    std::array<double, AIORS_METRIC_COUNT> values{};
    std::array<bool, AIORS_METRIC_COUNT> valid{};
    uint64_t uptime_s = 0;
    unsigned diagnostics_version = 0;
    std::string software_version;
    std::string software_revision;
    std::string hardware_version;
    std::string psu_gpio_status;
    std::string psu_a_state;
    std::string psu_a_fault;
    std::string psu_b_state;
    std::string psu_b_fault;
    std::string adc_status;
    std::string adc_primary_status;
    std::string adc_secondary_status;
    std::string fan_state;
    std::string environment_status;
    std::string digital_inputs_status;
    std::array<std::string, 4> digital_inputs;
    std::array<std::string, AIORS_STATUS_COUNT> statuses{{"na", "na", "na", "na", "na", "na", "na", "na"}};
    std::array<std::string, AIORS_RADIO_COUNT> radio_states{{"na", "na", "na", "na"}};
    std::string overall_status;
  };

  struct MetricAgg
  {
    double min = 0.0;
    double max = 0.0;
    double sum = 0.0;
    uint64_t count = 0;
  };

  struct StatusAgg
  {
    uint64_t fault_events = 0;
    uint64_t fault_samples = 0;
    double fault_s = 0.0;
  };

  struct AiorsTotals
  {
    std::array<StatusAgg, AIORS_STATUS_COUNT> statuses{};
    uint64_t success_samples = 0;
    uint64_t failure_samples = 0;
    uint64_t psu_a_state_changes = 0;
    uint64_t psu_b_state_changes = 0;
    uint64_t psu_a_fault_events = 0;
    uint64_t psu_b_fault_events = 0;
    uint64_t restart_events = 0;
    uint64_t degraded_samples = 0;
    uint64_t fault_samples = 0;
    double state_coverage_s = 0.0;
    double energy_coverage_s = 0.0;
    double psu_a_energy_coverage_s = 0.0;
    double psu_b_energy_coverage_s = 0.0;
    double psu_a_energy_wh = 0.0;
    double psu_b_energy_wh = 0.0;
    double psu_a_charge_ah = 0.0;
    double psu_b_charge_ah = 0.0;
    double psu_a_on_s = 0.0;
    double psu_a_off_s = 0.0;
    double psu_b_on_s = 0.0;
    double psu_b_off_s = 0.0;
    double psu_a_fault_s = 0.0;
    double psu_b_fault_s = 0.0;
    double degraded_s = 0.0;
    double fault_s = 0.0;
  };

  struct AiorsWindow
  {
    std::array<MetricAgg, AIORS_METRIC_COUNT> metrics{};
    std::array<StatusAgg, AIORS_STATUS_COUNT> statuses{};
    uint64_t success_samples = 0;
    uint64_t failure_samples = 0;
    uint64_t psu_a_state_changes = 0;
    uint64_t psu_b_state_changes = 0;
    uint64_t psu_a_fault_events = 0;
    uint64_t psu_b_fault_events = 0;
    uint64_t restart_events = 0;
    uint64_t degraded_samples = 0;
    uint64_t fault_samples = 0;
    double coverage_s = 0.0;
    double psu_a_off_s = 0.0;
    double psu_b_off_s = 0.0;
    double psu_a_fault_s = 0.0;
    double psu_b_fault_s = 0.0;
    double degraded_s = 0.0;
    double fault_s = 0.0;
  };

  bool parseAiorsDiagnostics(const std::string& text, AiorsSample& sample,
                             std::string& error) const;
  void updateAiorsTotals(const AiorsSample& sample);
  void trimAiorsHistory(double now_s);
  AiorsWindow aggregateAiorsWindow(double now_s, double window_s) const;
  void appendAiorsStats(std::ostringstream& os, double now_s) const;

  static constexpr size_t NBUCKET = 60;
  Bucket buckets[NBUCKET];
  size_t idx = 0;

  // current minute marker
  uint64_t cur_minute = 0;

  // active state tracking (start times as seconds since steady epoch)
  bool frn_tx_active = false;
  bool frn_rx_active = false;
  bool rf_tx_active = false;
  bool rf_rx_active = false;
  bool sql_active = false;
  double frn_tx_t0 = 0.0;
  double frn_rx_t0 = 0.0;
  double rf_tx_t0 = 0.0;
  double rf_rx_t0 = 0.0;
  double sql_t0 = 0.0;

  // totals since boot
  DurAgg frn_tx_total;
  DurAgg frn_rx_total;
  DurAgg rf_tx_total;
  DurAgg rf_rx_total;
  DurAgg sql_total;
  uint64_t frn_tx_bytes_total = 0;
  uint64_t frn_rx_bytes_total = 0;
  uint64_t frn_link_up_total = 0;
  uint64_t frn_link_down_total = 0;
  CmdAgg cmd_total;

  // FRN users
  uint64_t frn_users_cur = 0;
  uint64_t frn_users_peak_1h = 0;
  std::unordered_map<std::string, uint64_t> user_last_seen_minute; // for unique_1h
  std::unordered_map<std::string, bool> user_present;             // current set

  // timer
  Async::Timer* tick_timer = nullptr;
  uint32_t stats_interval_ms = 60000;
  std::string persist_path;
  std::string snapshot_path;
  std::string history_path;
  bool log_to_stdout = true;
  bool started = false;
  std::thread stats_writer;
  std::mutex stats_writer_mutex;
  std::condition_variable stats_writer_ready;
  bool stats_writer_stop = false;
  bool stats_snapshot_pending = false;
  bool stats_persist_pending = false;
  std::string stats_snapshot_line;
  std::string stats_persist_text;
  std::deque<std::string> stats_history_pending;

  // AIORS diagnostics collection and rolling 24-hour telemetry history.
  bool aiors_diagnostics_enabled = false;
  uint32_t aiors_sample_interval_s = 30;
  std::string aiorsctl_path = "/usr/local/bin/aiorsctl";
  Async::Timer* aiors_timer = nullptr;
  Async::Exec* aiors_exec = nullptr;
  std::string aiors_stdout;
  std::string aiors_stderr;
  std::deque<AiorsSample> aiors_samples;
  std::deque<double> aiors_failure_times;
  AiorsTotals aiors_totals;
  double last_aiors_attempt_monotonic = 0.0;
  double last_aiors_success_monotonic = 0.0;
  bool last_aiors_attempt_ok = false;
  std::string last_aiors_error = "not_started";
  std::string linux_boot_id = "unknown";

  // FRN activity timestamps (monotonic seconds). 0 means "unknown".
  double last_frn_rx_monotonic = 0.0;
  double last_frn_tx_monotonic = 0.0;
  double last_frn_link_up_monotonic = 0.0;
  double last_frn_link_down_monotonic = 0.0;

  // time base
  double start_monotonic = 0.0;
};

#endif // SVX_STATS_H
