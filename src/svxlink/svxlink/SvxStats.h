#ifndef SVX_STATS_H
#define SVX_STATS_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Async { class Timer; }

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
  SvxStats(const SvxStats&) = delete;
  SvxStats& operator=(const SvxStats&) = delete;

  void onTick(Async::Timer* t);
  void rotateBuckets(uint64_t now_minute);
  uint64_t uptimeSeconds() const;



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
  bool started = false;

  // FRN activity timestamps (monotonic seconds). 0 means "unknown".
  double last_frn_rx_monotonic = 0.0;
  double last_frn_tx_monotonic = 0.0;
  double last_frn_link_up_monotonic = 0.0;
  double last_frn_link_down_monotonic = 0.0;

  // time base
  double start_monotonic = 0.0;
};

#endif // SVX_STATS_H