#include "SvxStats.h"

#include <AsyncExec.h>
#include <AsyncTimer.h>
#include <config.h>
#include <version/SVXLINK.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <limits>

using std::string;

static inline double mono_now_sec()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static inline uint64_t minute_now()
{
  using namespace std::chrono;
  auto now = time_point_cast<minutes>(steady_clock::now());
  return (uint64_t) now.time_since_epoch().count();
}

static inline uint64_t unix_now_sec()
{
  using namespace std::chrono;
  return (uint64_t) duration_cast<seconds>(
      system_clock::now().time_since_epoch()).count();
}

static inline std::string trim_ascii(const std::string& value)
{
  const std::string whitespace = " \t\r\n";
  const size_t first = value.find_first_not_of(whitespace);
  if (first == std::string::npos)
    return std::string();
  const size_t last = value.find_last_not_of(whitespace);
  return value.substr(first, last - first + 1);
}

static inline std::string stats_token(const std::string& value)
{
  if (value.empty())
    return "unknown";

  std::string token = value;
  for (char& ch : token)
  {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (!(std::isalnum(uch) || ch == '.' || ch == '_' || ch == '-'))
      ch = '_';
  }
  return token;
}

static bool parse_u64_strict(const std::string& text, uint64_t& value)
{
  if (text.empty() || text[0] == '-')
    return false;

  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0')
    return false;
  value = static_cast<uint64_t>(parsed);
  return true;
}

static bool parse_double_strict(const std::string& text, double& value)
{
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      !std::isfinite(parsed))
    return false;
  value = parsed;
  return true;
}

SvxStats& SvxStats::instance()
{
  static SvxStats inst;
  return inst;
}

SvxStats::SvxStats()
{
  start_monotonic = mono_now_sec();
  {
    std::ifstream boot_id_file("/proc/sys/kernel/random/boot_id");
    std::string boot_id;
    if (boot_id_file.is_open() && std::getline(boot_id_file, boot_id))
      linux_boot_id = stats_token(trim_ascii(boot_id));
  }
  cur_minute = minute_now();
  // initialize buckets
  for (size_t i = 0; i < NBUCKET; ++i)
  {
    buckets[i].minute = cur_minute;
  }
  idx = 0;
}

SvxStats::~SvxStats()
{
  {
    std::lock_guard<std::mutex> lock(stats_writer_mutex);
    stats_writer_stop = true;
  }
  stats_writer_ready.notify_one();
  if (stats_writer.joinable())
    stats_writer.join();
}

void SvxStats::start()
{
  start(60);
}

void SvxStats::start(uint32_t interval_s)
{
  if (started)
    return;
  started = true;
  stats_writer = std::thread(&SvxStats::statsWriterLoop, this);

  stats_interval_ms = (interval_s == 0 ? 60000U : (uint32_t)(interval_s * 1000U));
  // NOTE: Async::Timer defaults to TYPE_ONESHOT. We want periodic stats.
  tick_timer = new Async::Timer(stats_interval_ms, Async::Timer::TYPE_PERIODIC, false);
  tick_timer->expired.connect(sigc::mem_fun(*this, &SvxStats::onTick));
  tick_timer->setEnable(true);

  if (aiors_diagnostics_enabled)
  {
    aiors_timer = new Async::Timer(aiors_sample_interval_s * 1000U,
                                   Async::Timer::TYPE_PERIODIC, false);
    aiors_timer->expired.connect(
        sigc::mem_fun(*this, &SvxStats::onAiorsSampleTimer));
    aiors_timer->setEnable(true);
    collectAiorsDiagnostics();
  }
}


uint64_t SvxStats::uptimeSeconds() const
{
  double now = mono_now_sec();
  if (now < start_monotonic) return 0;
  return (uint64_t) std::llround(now - start_monotonic);
}

void SvxStats::rotateBuckets(uint64_t now_minute)
{
  if (now_minute == cur_minute)
    return;

  // Advance minute-by-minute; reset bucket for each new minute
  while (cur_minute < now_minute)
  {
    cur_minute++;
    idx = (idx + 1) % NBUCKET;
    buckets[idx] = Bucket{};
    buckets[idx].minute = cur_minute;
  }
  for (auto it = user_last_seen_minute.begin(); it != user_last_seen_minute.end(); )
  {
    if (it->second + NBUCKET <= now_minute)
      it = user_last_seen_minute.erase(it);
    else
      ++it;
  }
}

static inline void duragg_add(SvxStats::DurAgg& a, double sec)
{
  a.evt++;
  a.sec += sec;
  a.sum_sec += sec;
  if (a.min_sec == 0.0 || sec < a.min_sec) a.min_sec = sec;
  if (sec > a.max_sec) a.max_sec = sec;
}

static inline void cmdagg_add_ms(SvxStats::CmdAgg& c, uint32_t ms)
{
  c.sum_ms += ms;
  c.cnt_ms += 1;
  if (ms > c.max_ms) c.max_ms = ms;
}

void SvxStats::onFrnClientListUpdate(const std::vector<std::string>& client_list)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  // Client list updates are received from the FRN server, so treat this as RX activity.
  last_frn_rx_monotonic = mono_now_sec();

  // Build new presence set
  std::unordered_map<std::string, bool> new_present;
  new_present.reserve(client_list.size());
  for (const auto& s : client_list)
  {
    new_present[s] = true;
    user_last_seen_minute[s] = m;
  }

  // Joins / leaves based on presence diff
  uint64_t joins = 0, leaves = 0;
  for (const auto& kv : new_present)
  {
    if (user_present.find(kv.first) == user_present.end())
      joins++;
  }
  for (const auto& kv : user_present)
  {
    if (new_present.find(kv.first) == new_present.end())
      leaves++;
  }

  buckets[idx].user_join += joins;
  buckets[idx].user_leave += leaves;

  user_present.swap(new_present);
  frn_users_cur = user_present.size();

  // Peak tracking over last hour: compute fresh on tick (cheap) but also update quickly
  // We'll just update with current and let onTick compute accurate peak.
}

void SvxStats::onFrnLinkUp()
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  buckets[idx].frn_link_up += 1;
  frn_link_up_total += 1;
  last_frn_link_up_monotonic = mono_now_sec();
}

void SvxStats::onFrnLinkDown()
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  buckets[idx].frn_link_down += 1;
  frn_link_down_total += 1;
  last_frn_link_down_monotonic = mono_now_sec();
}

void SvxStats::onFrnTxState(bool is_tx)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  double now = mono_now_sec();

  if (is_tx && !frn_tx_active)
  {
    frn_tx_active = true;
    frn_tx_t0 = now;
  }
  else if (!is_tx && frn_tx_active)
  {
    frn_tx_active = false;
    double sec = std::max(0.0, now - frn_tx_t0);
    duragg_add(buckets[idx].frn_tx, sec);
    duragg_add(frn_tx_total, sec);
  }
}

void SvxStats::onFrnRxState(bool is_rx)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  double now = mono_now_sec();

  if (is_rx && !frn_rx_active)
  {
    frn_rx_active = true;
    frn_rx_t0 = now;
  }
  else if (!is_rx && frn_rx_active)
  {
    frn_rx_active = false;
    double sec = std::max(0.0, now - frn_rx_t0);
    duragg_add(buckets[idx].frn_rx, sec);
    duragg_add(frn_rx_total, sec);
  }
}

void SvxStats::addFrnTxBytes(uint64_t bytes)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  last_frn_tx_monotonic = mono_now_sec();
  buckets[idx].frn_tx_bytes += bytes;
  frn_tx_bytes_total += bytes;
}

void SvxStats::addFrnRxBytes(uint64_t bytes)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  last_frn_rx_monotonic = mono_now_sec();
  buckets[idx].frn_rx_bytes += bytes;
  frn_rx_bytes_total += bytes;
}

void SvxStats::onSquelchState(bool is_open)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  double now = mono_now_sec();

  if (is_open && !sql_active)
  {
    sql_active = true;
    sql_t0 = now;
  }
  else if (!is_open && sql_active)
  {
    sql_active = false;
    double sec = std::max(0.0, now - sql_t0);
    duragg_add(buckets[idx].sql, sec);
    duragg_add(sql_total, sec);
  }
}

void SvxStats::onRfTxState(bool is_tx)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  double now = mono_now_sec();

  if (is_tx && !rf_tx_active)
  {
    rf_tx_active = true;
    rf_tx_t0 = now;
  }
  else if (!is_tx && rf_tx_active)
  {
    rf_tx_active = false;
    double sec = std::max(0.0, now - rf_tx_t0);
    duragg_add(buckets[idx].rf_tx, sec);
    duragg_add(rf_tx_total, sec);
  }
}


void SvxStats::onRfRxState(bool is_rx)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  double now = mono_now_sec();

  if (is_rx && !rf_rx_active)
  {
    rf_rx_active = true;
    rf_rx_t0 = now;
  }
  else if (!is_rx && rf_rx_active)
  {
    rf_rx_active = false;
    double sec = std::max(0.0, now - rf_rx_t0);
    duragg_add(buckets[idx].rf_rx, sec);
    duragg_add(rf_rx_total, sec);
  }
}

void SvxStats::onCmdAccepted()
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  buckets[idx].cmd.ok++;
  cmd_total.ok++;
}

void SvxStats::onCmdRejected()
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  buckets[idx].cmd.rej++;
  cmd_total.rej++;
}

void SvxStats::onCmdAuthFailed()
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  buckets[idx].cmd.authfail++;
  cmd_total.authfail++;
}

void SvxStats::onCmdBroadcastAttempt()
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  buckets[idx].cmd.bc_attempt++;
  cmd_total.bc_attempt++;
}

void SvxStats::onCmdExecTimeMs(uint32_t ms)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
  cmdagg_add_ms(buckets[idx].cmd, ms);
  cmdagg_add_ms(cmd_total, ms);
}

static inline void duragg_merge(SvxStats::DurAgg& out, const SvxStats::DurAgg& in)
{
  out.evt += in.evt;
  out.sec += in.sec;
  out.sum_sec += in.sum_sec;
  if (in.min_sec > 0.0 && (out.min_sec == 0.0 || in.min_sec < out.min_sec)) out.min_sec = in.min_sec;
  if (in.max_sec > out.max_sec) out.max_sec = in.max_sec;
}

static inline void cmdagg_merge(SvxStats::CmdAgg& out, const SvxStats::CmdAgg& in)
{
  out.ok += in.ok;
  out.rej += in.rej;
  out.authfail += in.authfail;
  out.bc_attempt += in.bc_attempt;
  out.sum_ms += in.sum_ms;
  out.cnt_ms += in.cnt_ms;
  if (in.max_ms > out.max_ms) out.max_ms = in.max_ms;
}

std::string SvxStats::formatStatsLine()
{
  uint64_t now_m = minute_now();
  rotateBuckets(now_m);
  double now_s = mono_now_sec();

  // Aggregate last hour (60 buckets)
  DurAgg frn_tx_1h, frn_rx_1h, rf_tx_1h, rf_rx_1h;
  CmdAgg cmd_1h;
  uint64_t frn_tx_bytes_1h = 0, frn_rx_bytes_1h = 0;
  uint64_t join_1h = 0, leave_1h = 0;
  uint64_t link_up_1h = 0, link_down_1h = 0;


  for (size_t i = 0; i < NBUCKET; ++i)
  {
    const Bucket& b = buckets[i];
    // only include last 60 minutes, including current
    if (b.minute + NBUCKET <= now_m) continue;
    duragg_merge(frn_tx_1h, b.frn_tx);
    duragg_merge(frn_rx_1h, b.frn_rx);
    duragg_merge(rf_tx_1h, b.rf_tx);
    duragg_merge(rf_rx_1h, b.rf_rx);
    cmdagg_merge(cmd_1h, b.cmd);
    frn_tx_bytes_1h += b.frn_tx_bytes;
    frn_rx_bytes_1h += b.frn_rx_bytes;
    link_up_1h += b.frn_link_up;
    link_down_1h += b.frn_link_down;
    join_1h += b.user_join;
    leave_1h += b.user_leave;
  }

  // Add ongoing active durations into the 1h and total seconds (as partial)
  auto add_active = [&](bool active, double t0, DurAgg& oneh, DurAgg& total)
  {
    if (!active) return;
    double sec = std::max(0.0, now_s - t0);
    oneh.sec += sec;
    total.sec += sec;
  };
  DurAgg frn_tx_total_eff = frn_tx_total;
  DurAgg frn_rx_total_eff = frn_rx_total;
  DurAgg rf_tx_total_eff  = rf_tx_total;
  DurAgg rf_rx_total_eff  = rf_rx_total;

  add_active(frn_tx_active, frn_tx_t0, frn_tx_1h, frn_tx_total_eff);
  add_active(frn_rx_active, frn_rx_t0, frn_rx_1h, frn_rx_total_eff);
  add_active(rf_tx_active,  rf_tx_t0,  rf_tx_1h,  rf_tx_total_eff);
  add_active(rf_rx_active,  rf_rx_t0,  rf_rx_1h,  rf_rx_total_eff);

  // unique users in last hour
  uint64_t unique_1h = 0;
  for (const auto& kv : user_last_seen_minute)
  {
    if (kv.second + NBUCKET > now_m) unique_1h++;
  }

  // peak users last hour: recompute as max of frn_users_cur and previous stored peak
  // (Updated in onTick)
  uint64_t peak_1h = frn_users_peak_1h;

  // Derived: TX/RX hours total
  double rf_tx_hours_total = rf_tx_total_eff.sec / 3600.0;
  double rf_rx_hours_total = rf_rx_total_eff.sec / 3600.0;

  // Derived duty cycles
  auto pct = [](double sec) -> double { return std::min(100.0, std::max(0.0, sec * 100.0 / 3600.0)); };

  // FRN link/activity ages (seconds since last event; -1 if unknown)
  auto age_s = [&](double last) -> int64_t {
    if (last <= 0.0) return -1;
    return (int64_t)std::llround(std::max(0.0, now_s - last));
  };
  const int64_t frn_rx_age_s = age_s(last_frn_rx_monotonic);
  const int64_t frn_tx_age_s = age_s(last_frn_tx_monotonic);
  const int64_t frn_link_up_age_s = age_s(last_frn_link_up_monotonic);
  const int64_t frn_link_down_age_s = age_s(last_frn_link_down_monotonic);

  std::ostringstream os;
  os.setf(std::ios::fixed);
  os << "STATS"
     << " stats_schema=3"
     << " ts_unix_s=" << unix_now_sec()
     << " uptime_s=" << uptimeSeconds()
     << " linux_uptime_s=" << linuxUptimeSeconds()
     << " svxlink_uptime_s=" << uptimeSeconds()
     << " svxlink_software_version=" << stats_token(SVXLINK_APP_VERSION)
     << " svxlink_software_revision=" << stats_token(PROJECT_VERSION)
     << " linux_boot_id=" << linux_boot_id
     << " frn_link_up_evt_1h=" << link_up_1h
     << " frn_link_down_evt_1h=" << link_down_1h
     << " frn_link_up_total=" << frn_link_up_total
     << " frn_link_down_total=" << frn_link_down_total
     << " frn_link_up_age_s=" << frn_link_up_age_s
     << " frn_link_down_age_s=" << frn_link_down_age_s
     << " frn_rx_age_s=" << frn_rx_age_s
     << " frn_tx_age_s=" << frn_tx_age_s
     << " frn_users=" << frn_users_cur
     << " frn_users_peak_1h=" << peak_1h
     << " frn_users_unique_1h=" << unique_1h
     << " frn_link_up_1h=" << link_up_1h
     << " frn_link_down_1h=" << link_down_1h
     << " frn_user_join_1h=" << join_1h
     << " frn_user_leave_1h=" << leave_1h

     << " frn_tx_evt_1h=" << frn_tx_1h.evt
     << " frn_tx_s_1h=" << (uint64_t)std::llround(frn_tx_1h.sec)
     << " frn_tx_min_s_1h=" << (frn_tx_1h.min_sec > 0.0 ? frn_tx_1h.min_sec : 0.0)
     << " frn_tx_max_s_1h=" << frn_tx_1h.max_sec
     << " frn_tx_bytes_1h=" << frn_tx_bytes_1h
     << " frn_tx_duty_1h=" << pct(frn_tx_1h.sec)

     << " frn_rx_evt_1h=" << frn_rx_1h.evt
     << " frn_rx_s_1h=" << (uint64_t)std::llround(frn_rx_1h.sec)
     << " frn_rx_min_s_1h=" << (frn_rx_1h.min_sec > 0.0 ? frn_rx_1h.min_sec : 0.0)
     << " frn_rx_max_s_1h=" << frn_rx_1h.max_sec
     << " frn_rx_bytes_1h=" << frn_rx_bytes_1h

     << " rf_tx_evt_1h=" << rf_tx_1h.evt
     << " rf_tx_s_1h=" << (uint64_t)std::llround(rf_tx_1h.sec)
     << " rf_tx_min_s_1h=" << (rf_tx_1h.min_sec > 0.0 ? rf_tx_1h.min_sec : 0.0)
     << " rf_tx_max_s_1h=" << rf_tx_1h.max_sec
     << " rf_tx_hours_total=" << rf_tx_hours_total
     << " rf_tx_duty_1h=" << pct(rf_tx_1h.sec)

     << " rf_rx_evt_1h=" << rf_rx_1h.evt
     << " rf_rx_s_1h=" << (uint64_t)std::llround(rf_rx_1h.sec)
     << " rf_rx_min_s_1h=" << (rf_rx_1h.min_sec > 0.0 ? rf_rx_1h.min_sec : 0.0)
     << " rf_rx_max_s_1h=" << rf_rx_1h.max_sec
     << " rf_rx_hours_total=" << rf_rx_hours_total
     << " rf_rx_duty_1h=" << pct(rf_rx_1h.sec)

     << " cmd_ok_1h=" << cmd_1h.ok
     << " cmd_rej_1h=" << cmd_1h.rej
     << " cmd_authfail_1h=" << cmd_1h.authfail
     << " cmd_bc_attempt_1h=" << cmd_1h.bc_attempt
     << " cmd_avg_ms_1h=" << (cmd_1h.cnt_ms ? (cmd_1h.sum_ms / cmd_1h.cnt_ms) : 0)
     << " cmd_max_ms_1h=" << cmd_1h.max_ms
     ;

  appendAiorsStats(os, now_s);
  return os.str();
}

std::vector<std::string> SvxStats::formatStatsGroups()
{
  uint64_t now_m = minute_now();
  rotateBuckets(now_m);
  double now_s = mono_now_sec();

  DurAgg frn_tx_1h, frn_rx_1h, rf_tx_1h, rf_rx_1h;
  CmdAgg cmd_1h;
  uint64_t frn_tx_bytes_1h = 0, frn_rx_bytes_1h = 0;
  uint64_t join_1h = 0, leave_1h = 0;
  uint64_t link_up_1h = 0, link_down_1h = 0;

  for (size_t i = 0; i < NBUCKET; ++i)
  {
    const Bucket& b = buckets[i];
    if (b.minute + NBUCKET <= now_m) continue;
    duragg_merge(frn_tx_1h, b.frn_tx);
    duragg_merge(frn_rx_1h, b.frn_rx);
    duragg_merge(rf_tx_1h, b.rf_tx);
    duragg_merge(rf_rx_1h, b.rf_rx);
    cmdagg_merge(cmd_1h, b.cmd);
    frn_tx_bytes_1h += b.frn_tx_bytes;
    frn_rx_bytes_1h += b.frn_rx_bytes;
    link_up_1h += b.frn_link_up;
    link_down_1h += b.frn_link_down;
    join_1h += b.user_join;
    leave_1h += b.user_leave;
  }

  auto add_active = [&](bool active, double t0, DurAgg& oneh, DurAgg& total)
  {
    if (!active) return;
    double sec = std::max(0.0, now_s - t0);
    oneh.sec += sec;
    total.sec += sec;
  };

  DurAgg frn_tx_total_eff = frn_tx_total;
  DurAgg frn_rx_total_eff = frn_rx_total;
  DurAgg rf_tx_total_eff  = rf_tx_total;
  DurAgg rf_rx_total_eff  = rf_rx_total;

  add_active(frn_tx_active, frn_tx_t0, frn_tx_1h, frn_tx_total_eff);
  add_active(frn_rx_active, frn_rx_t0, frn_rx_1h, frn_rx_total_eff);
  add_active(rf_tx_active,  rf_tx_t0,  rf_tx_1h,  rf_tx_total_eff);
  add_active(rf_rx_active,  rf_rx_t0,  rf_rx_1h,  rf_rx_total_eff);

  uint64_t unique_1h = 0;
  for (const auto& kv : user_last_seen_minute)
  {
    if (kv.second + NBUCKET > now_m) unique_1h++;
  }
  uint64_t peak_1h = frn_users_peak_1h;

  auto pct = [](double sec) -> double { return std::min(100.0, std::max(0.0, sec * 100.0 / 3600.0)); };
  auto sec_u64 = [](double sec) -> uint64_t { return (uint64_t)std::llround(std::max(0.0, sec)); };

  auto age_s = [&](double last) -> int64_t {
    if (last <= 0.0) return -1;
    return (int64_t)std::llround(std::max(0.0, now_s - last));
  };
  const int64_t frn_rx_age_s = age_s(last_frn_rx_monotonic);
  const int64_t frn_tx_age_s = age_s(last_frn_tx_monotonic);
  const int64_t frn_link_up_age_s = age_s(last_frn_link_up_monotonic);
  const int64_t frn_link_down_age_s = age_s(last_frn_link_down_monotonic);

  std::vector<std::string> out;
  out.reserve(8);

  // General / users
  {
    std::ostringstream os;
    os << "STATS uptime=" << uptimeSeconds() << "s"
       << " users=" << frn_users_cur
       << " peak1h=" << peak_1h
       << " uniq1h=" << unique_1h
       << " linkup1h=" << link_up_1h
       << " linkdown1h=" << link_down_1h
       << " linkup_total=" << frn_link_up_total
       << " linkdown_total=" << frn_link_down_total
       << " linkup_age=" << frn_link_up_age_s << "s"
       << " linkdown_age=" << frn_link_down_age_s << "s"
       << " rx_age=" << frn_rx_age_s << "s"
       << " tx_age=" << frn_tx_age_s << "s"
       << " join1h=" << join_1h
       << " leave1h=" << leave_1h;
    out.push_back(os.str());
  }

  // FRN TX
  {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << "FRN TX 1h evt=" << frn_tx_1h.evt
       << " t=" << sec_u64(frn_tx_1h.sec) << "s"
       << " min=" << (frn_tx_1h.min_sec > 0.0 ? frn_tx_1h.min_sec : 0.0) << "s"
       << " max=" << frn_tx_1h.max_sec << "s"
       << " duty=" << pct(frn_tx_1h.sec) << "% "
       << "bytes=" << frn_tx_bytes_1h;
    out.push_back(os.str());
  }

  // FRN RX
  {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << "FRN RX 1h evt=" << frn_rx_1h.evt
       << " t=" << sec_u64(frn_rx_1h.sec) << "s"
       << " min=" << (frn_rx_1h.min_sec > 0.0 ? frn_rx_1h.min_sec : 0.0) << "s"
       << " max=" << frn_rx_1h.max_sec << "s"
       << " duty=" << pct(frn_rx_1h.sec) << "% "
       << "bytes=" << frn_rx_bytes_1h;
    out.push_back(os.str());
  }

  // RF TX
  {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << "RF TX 1h evt=" << rf_tx_1h.evt
       << " t=" << sec_u64(rf_tx_1h.sec) << "s"
       << " min=" << (rf_tx_1h.min_sec > 0.0 ? rf_tx_1h.min_sec : 0.0) << "s"
       << " max=" << rf_tx_1h.max_sec << "s"
       << " duty=" << pct(rf_tx_1h.sec) << "% "
       << "total=" << (rf_tx_total_eff.sec / 3600.0) << "h";
    out.push_back(os.str());
  }

  // RF RX (squelch-open based)
  {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << "RF RX 1h evt=" << rf_rx_1h.evt
       << " t=" << sec_u64(rf_rx_1h.sec) << "s"
       << " min=" << (rf_rx_1h.min_sec > 0.0 ? rf_rx_1h.min_sec : 0.0) << "s"
       << " max=" << rf_rx_1h.max_sec << "s"
       << " duty=" << pct(rf_rx_1h.sec) << "% "
       << "total=" << (rf_rx_total_eff.sec / 3600.0) << "h";
    out.push_back(os.str());
  }

  // CMD
  {
    std::ostringstream os;
    os << "CMD 1h ok=" << cmd_1h.ok
       << " rej=" << cmd_1h.rej
       << " auth=" << cmd_1h.authfail
       << " bc=" << cmd_1h.bc_attempt
       << " avg=" << (cmd_1h.cnt_ms ? (cmd_1h.sum_ms / cmd_1h.cnt_ms) : 0) << "ms"
       << " max=" << cmd_1h.max_ms << "ms";
    out.push_back(os.str());
  }

  return out;
}


void SvxStats::onTick(Async::Timer* /*t*/)
{
  // Update peak users in last hour: use current users as a sample
  // This is a conservative peak, but for practical use it's fine. To get exact,
  // we'd need store user counts per minute. We'll do that quickly by keeping peak.
  uint64_t now_m = minute_now();
  rotateBuckets(now_m);

  // Track peak in a rolling manner: we need reset when hour window moves.
  // We recompute peak by scanning buckets would require stored counts, so instead
  // we approximate by decaying peak when no client list updates. We'll recompute
  // as max(current, previous_peak) and reset once per hour boundary by using
  // a simple guard: if peak is older than 60m, reset. We'll store peak minute in map.
  static uint64_t peak_set_minute = 0;
  if (peak_set_minute + NBUCKET <= now_m)
  {
    frn_users_peak_1h = frn_users_cur;
    peak_set_minute = now_m;
  }
  if (frn_users_cur > frn_users_peak_1h)
  {
    frn_users_peak_1h = frn_users_cur;
    peak_set_minute = now_m;
  }

  const std::string line = formatStatsLine();
  if (log_to_stdout)
  {
    std::cout << line << std::endl;
  }
  {
    std::lock_guard<std::mutex> lock(stats_writer_mutex);
    stats_snapshot_line = line;
    stats_snapshot_pending = true;
    if (stats_history_pending.size() >= 64)
      stats_history_pending.pop_front();
    stats_history_pending.push_back(line);
  }
  stats_writer_ready.notify_one();
  savePersistedTotals();
}

int64_t SvxStats::linuxUptimeSeconds() const
{
  std::ifstream uptime_file("/proc/uptime");
  double uptime_s = 0.0;
  if (!(uptime_file >> uptime_s) || !std::isfinite(uptime_s) || uptime_s < 0.0)
    return -1;
  return static_cast<int64_t>(uptime_s);
}

void SvxStats::onAiorsSampleTimer(Async::Timer* /*t*/)
{
  collectAiorsDiagnostics();
}

void SvxStats::collectAiorsDiagnostics()
{
  if (!aiors_diagnostics_enabled || aiors_exec != nullptr)
    return;

  if (aiorsctl_path.empty())
  {
    recordAiorsFailure("empty_aiorsctl_path");
    return;
  }

  aiors_stdout.clear();
  aiors_stderr.clear();
  last_aiors_attempt_monotonic = mono_now_sec();

  aiors_exec = new Async::Exec(aiorsctl_path);
  aiors_exec->appendArgument("get");
  aiors_exec->appendArgument("diagnostics");
  aiors_exec->appendArgument("all");
  aiors_exec->setTimeout(3);

  aiors_exec->stdoutData.connect(
      [this](const char* buf, int cnt)
      {
        static const size_t MAX_OUTPUT = 65536;
        if (cnt > 0 && aiors_stdout.size() < MAX_OUTPUT)
        {
          const size_t append_count = std::min<size_t>(
              static_cast<size_t>(cnt), MAX_OUTPUT - aiors_stdout.size());
          aiors_stdout.append(buf, append_count);
        }
      });
  aiors_exec->stderrData.connect(
      [this](const char* buf, int cnt)
      {
        static const size_t MAX_ERROR = 4096;
        if (cnt > 0 && aiors_stderr.size() < MAX_ERROR)
        {
          const size_t append_count = std::min<size_t>(
              static_cast<size_t>(cnt), MAX_ERROR - aiors_stderr.size());
          aiors_stderr.append(buf, append_count);
        }
      });
  aiors_exec->exited.connect(
      [this](void)
      {
        onAiorsDiagnosticsExited();
      });

  if (!aiors_exec->run())
  {
    delete aiors_exec;
    aiors_exec = nullptr;
    recordAiorsFailure("exec_start_failed");
  }
}

void SvxStats::onAiorsDiagnosticsExited()
{
  if (aiors_exec == nullptr)
    return;

  const bool exited = aiors_exec->ifExited();
  const int exit_status = exited ? aiors_exec->exitStatus() : -1;
  delete aiors_exec;
  aiors_exec = nullptr;

  if (!exited || exit_status != 0)
  {
    std::ostringstream reason;
    reason << (exited ? "exit_" : "signal") << exit_status;
    recordAiorsFailure(reason.str());
    return;
  }

  AiorsSample sample;
  std::string error;
  if (!parseAiorsDiagnostics(aiors_stdout, sample, error))
  {
    recordAiorsFailure(error.empty() ? "parse_failed" : error);
    return;
  }

  const double now_s = mono_now_sec();
  sample.monotonic_s = now_s;
  sample.unix_s = unix_now_sec();
  updateAiorsTotals(sample);
  aiors_samples.push_back(sample);
  last_aiors_attempt_monotonic = now_s;
  last_aiors_success_monotonic = now_s;
  last_aiors_attempt_ok = true;
  last_aiors_error = "none";
  trimAiorsHistory(now_s);
}

void SvxStats::recordAiorsFailure(const std::string& reason)
{
  const double now_s = mono_now_sec();
  last_aiors_attempt_monotonic = now_s;
  last_aiors_attempt_ok = false;
  last_aiors_error = stats_token(reason);
  ++aiors_totals.failure_samples;
  aiors_failure_times.push_back(now_s);
  trimAiorsHistory(now_s);
}

bool SvxStats::parseAiorsDiagnostics(const std::string& text,
                                     AiorsSample& sample,
                                     std::string& error) const
{
  std::unordered_map<std::string, std::string> fields;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line))
  {
    line = trim_ascii(line);
    if (line.empty())
      continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0)
    {
      error = "invalid_line";
      return false;
    }
    const std::string key = trim_ascii(line.substr(0, eq));
    const std::string value = trim_ascii(line.substr(eq + 1));
    if (key.empty() || value.empty() || fields.find(key) != fields.end())
    {
      error = "invalid_field";
      return false;
    }
    fields[key] = value;
  }

  auto get_required = [&](const char* key, std::string& value) -> bool
  {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty())
    {
      error = std::string("missing_") + key;
      return false;
    }
    value = it->second;
    return true;
  };

  std::string version_text;
  uint64_t version = 0;
  if (!get_required("diagnostics_version", version_text) ||
      !parse_u64_strict(version_text, version) || version < 4 ||
      version > std::numeric_limits<unsigned>::max())
  {
    error = "unsupported_diagnostics_version";
    return false;
  }
  sample.diagnostics_version = static_cast<unsigned>(version);

  if (!get_required("software_version", sample.software_version) ||
      !get_required("software_revision", sample.software_revision) ||
      !get_required("hardware_version", sample.hardware_version) ||
      !get_required("psu_gpio_status", sample.psu_gpio_status) ||
      !get_required("psu_a_state", sample.psu_a_state) ||
      !get_required("psu_a_fault", sample.psu_a_fault) ||
      !get_required("psu_b_state", sample.psu_b_state) ||
      !get_required("psu_b_fault", sample.psu_b_fault) ||
      !get_required("adc_status", sample.adc_status) ||
      !get_required("adc_primary_status", sample.adc_primary_status) ||
      !get_required("adc_secondary_status", sample.adc_secondary_status) ||
      !get_required("fan_state", sample.fan_state) ||
      !get_required("environment_status", sample.environment_status) ||
      !get_required("digital_inputs_status", sample.digital_inputs_status) ||
      !get_required("di0", sample.digital_inputs[0]) ||
      !get_required("di1", sample.digital_inputs[1]) ||
      !get_required("di2", sample.digital_inputs[2]) ||
      !get_required("di3", sample.digital_inputs[3]) ||
      !get_required("overall_status", sample.overall_status))
    return false;

  struct StatusField
  {
    const char* key;
    AiorsStatus status;
  };
  static const StatusField status_fields[] =
  {
    {"sa818_a_status", AIORS_STATUS_SA818_A},
    {"sa818_b_status", AIORS_STATUS_SA818_B},
    {"cm108_a_status", AIORS_STATUS_CM108_A},
    {"cm108_b_status", AIORS_STATUS_CM108_B}
  };
  for (const StatusField& field : status_fields)
  {
    std::string& value = sample.statuses[field.status];
    if (!get_required(field.key, value))
      return false;
    if (value != "ok" && value != "fault")
    {
      error = std::string("invalid_") + field.key;
      return false;
    }
  }

  struct MetricField
  {
    const char* key;
    AiorsMetric metric;
  };
  static const MetricField metric_fields[] =
  {
    {"psu_a_input_voltage_v", AIORS_PSU_A_VOLTAGE},
    {"psu_input_current_a", AIORS_PSU_A_CURRENT},
    {"psu_b_input_voltage_v", AIORS_PSU_B_VOLTAGE},
    {"psu_input_current_b", AIORS_PSU_B_CURRENT},
    {"bus_3v3_voltage_v", AIORS_BUS_3V3_VOLTAGE},
    {"bus_5v_voltage_v", AIORS_BUS_5V_VOLTAGE},
    {"rf_a_average_voltage_v", AIORS_RF_A_VOLTAGE},
    {"rf_b_average_voltage_v", AIORS_RF_B_VOLTAGE},
    {"temperature_c", AIORS_TEMPERATURE},
    {"humidity_rh", AIORS_HUMIDITY},
    {"ai0_voltage_v", AIORS_AI0_VOLTAGE},
    {"ai1_voltage_v", AIORS_AI1_VOLTAGE},
    {"ai2_voltage_v", AIORS_AI2_VOLTAGE},
    {"ai3_voltage_v", AIORS_AI3_VOLTAGE}
  };

  for (const MetricField& field : metric_fields)
  {
    std::string value_text;
    if (!get_required(field.key, value_text))
      return false;
    if (value_text == "na")
      continue;

    double value = 0.0;
    if (!parse_double_strict(value_text, value))
    {
      error = std::string("invalid_") + field.key;
      return false;
    }
    sample.values[field.metric] = value;
    sample.valid[field.metric] = true;
  }

  std::string uptime_text;
  if (!get_required("aiors_uptime_s", uptime_text) ||
      !parse_u64_strict(uptime_text, sample.uptime_s))
  {
    error = "invalid_aiors_uptime_s";
    return false;
  }

  // HW 1.0 exposes one common input-current measurement, so per-channel
  // power would be misleading. HW 1.1 has independent A/B current channels.
  if (sample.hardware_version == "1.1")
  {
    if (sample.valid[AIORS_PSU_A_VOLTAGE] &&
        sample.valid[AIORS_PSU_A_CURRENT])
    {
      sample.values[AIORS_PSU_A_POWER] =
          sample.values[AIORS_PSU_A_VOLTAGE] *
          sample.values[AIORS_PSU_A_CURRENT];
      sample.valid[AIORS_PSU_A_POWER] = true;
    }
    if (sample.valid[AIORS_PSU_B_VOLTAGE] &&
        sample.valid[AIORS_PSU_B_CURRENT])
    {
      sample.values[AIORS_PSU_B_POWER] =
          sample.values[AIORS_PSU_B_VOLTAGE] *
          sample.values[AIORS_PSU_B_CURRENT];
      sample.valid[AIORS_PSU_B_POWER] = true;
    }
  }

  return true;
}

void SvxStats::updateAiorsTotals(const AiorsSample& sample)
{
  ++aiors_totals.success_samples;
  if (sample.overall_status == "degraded")
    ++aiors_totals.degraded_samples;
  else if (sample.overall_status == "fault")
    ++aiors_totals.fault_samples;

  for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
  {
    if (sample.statuses[status] == "fault")
      ++aiors_totals.statuses[status].fault_samples;
  }

  if (aiors_samples.empty())
    return;

  const AiorsSample& previous = aiors_samples.back();
  if (sample.uptime_s < previous.uptime_s)
    ++aiors_totals.restart_events;

  const double gap_s = sample.monotonic_s - previous.monotonic_s;
  const double stale_after_s = std::max(5.0,
      static_cast<double>(aiors_sample_interval_s) * 3.0);
  const bool continuous = gap_s >= 0.0 && gap_s <= stale_after_s &&
      sample.uptime_s >= previous.uptime_s;
  if (!continuous)
    return;

  aiors_totals.state_coverage_s += gap_s;
  if (previous.psu_a_state == "on")
    aiors_totals.psu_a_on_s += gap_s;
  else if (previous.psu_a_state == "off")
    aiors_totals.psu_a_off_s += gap_s;
  if (previous.psu_b_state == "on")
    aiors_totals.psu_b_on_s += gap_s;
  else if (previous.psu_b_state == "off")
    aiors_totals.psu_b_off_s += gap_s;
  if (previous.psu_a_fault == "yes")
    aiors_totals.psu_a_fault_s += gap_s;
  if (previous.psu_b_fault == "yes")
    aiors_totals.psu_b_fault_s += gap_s;
  if (previous.overall_status == "degraded")
    aiors_totals.degraded_s += gap_s;
  else if (previous.overall_status == "fault")
    aiors_totals.fault_s += gap_s;

  if (sample.psu_a_state != previous.psu_a_state)
    ++aiors_totals.psu_a_state_changes;
  if (sample.psu_b_state != previous.psu_b_state)
    ++aiors_totals.psu_b_state_changes;
  if (sample.psu_a_fault == "yes" && previous.psu_a_fault != "yes")
    ++aiors_totals.psu_a_fault_events;
  if (sample.psu_b_fault == "yes" && previous.psu_b_fault != "yes")
    ++aiors_totals.psu_b_fault_events;

  for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
  {
    StatusAgg& total = aiors_totals.statuses[status];
    if (previous.statuses[status] == "fault")
      total.fault_s += gap_s;
    if (sample.statuses[status] == "fault" &&
        previous.statuses[status] != "fault")
    {
      ++total.fault_events;
    }
  }

  const double elapsed_hours = gap_s / 3600.0;
  auto integrate_channel = [&](AiorsMetric current_metric,
                               AiorsMetric power_metric,
                               double& charge_ah,
                               double& energy_wh,
                               double& coverage_s) -> bool
  {
    if (!previous.valid[current_metric] || !sample.valid[current_metric] ||
        !previous.valid[power_metric] || !sample.valid[power_metric])
    {
      return false;
    }

    const double average_current = std::max(0.0,
        (previous.values[current_metric] + sample.values[current_metric]) / 2.0);
    const double average_power = std::max(0.0,
        (previous.values[power_metric] + sample.values[power_metric]) / 2.0);
    charge_ah += average_current * elapsed_hours;
    energy_wh += average_power * elapsed_hours;
    coverage_s += gap_s;
    return true;
  };

  const bool channel_a_integrated = integrate_channel(
      AIORS_PSU_A_CURRENT, AIORS_PSU_A_POWER,
      aiors_totals.psu_a_charge_ah, aiors_totals.psu_a_energy_wh,
      aiors_totals.psu_a_energy_coverage_s);
  const bool channel_b_integrated = integrate_channel(
      AIORS_PSU_B_CURRENT, AIORS_PSU_B_POWER,
      aiors_totals.psu_b_charge_ah, aiors_totals.psu_b_energy_wh,
      aiors_totals.psu_b_energy_coverage_s);
  if (channel_a_integrated && channel_b_integrated)
    aiors_totals.energy_coverage_s += gap_s;
}

void SvxStats::trimAiorsHistory(double now_s)
{
  const double cutoff = now_s - 86400.0;
  // Keep one predecessor sample so state duration at the 24-hour boundary can
  // be calculated without claiming coverage before the last observed state.
  while (aiors_samples.size() > 1 && aiors_samples[1].monotonic_s < cutoff)
    aiors_samples.pop_front();
  while (!aiors_failure_times.empty() && aiors_failure_times.front() < cutoff)
    aiors_failure_times.pop_front();
}

SvxStats::AiorsWindow SvxStats::aggregateAiorsWindow(double now_s,
                                                     double window_s) const
{
  AiorsWindow out;
  const double cutoff = now_s - window_s;
  const double stale_after_s = std::max(5.0,
      static_cast<double>(aiors_sample_interval_s) * 3.0);

  for (size_t i = 0; i < aiors_samples.size(); ++i)
  {
    const AiorsSample& sample = aiors_samples[i];
    if (sample.monotonic_s >= cutoff)
    {
      ++out.success_samples;
      if (sample.overall_status == "degraded")
        ++out.degraded_samples;
      else if (sample.overall_status == "fault")
        ++out.fault_samples;

      for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
      {
        if (sample.statuses[status] == "fault")
          ++out.statuses[status].fault_samples;
      }

      for (size_t metric = 0; metric < AIORS_METRIC_COUNT; ++metric)
      {
        if (!sample.valid[metric])
          continue;
        MetricAgg& agg = out.metrics[metric];
        const double value = sample.values[metric];
        if (agg.count == 0)
        {
          agg.min = value;
          agg.max = value;
        }
        else
        {
          agg.min = std::min(agg.min, value);
          agg.max = std::max(agg.max, value);
        }
        agg.sum += value;
        ++agg.count;
      }
    }

    if (i > 0 && sample.monotonic_s >= cutoff)
    {
      const AiorsSample& previous = aiors_samples[i - 1];
      const double gap_s = sample.monotonic_s - previous.monotonic_s;
      if (gap_s >= 0.0 && gap_s <= stale_after_s)
      {
        if (sample.psu_a_state != previous.psu_a_state)
          ++out.psu_a_state_changes;
        if (sample.psu_b_state != previous.psu_b_state)
          ++out.psu_b_state_changes;
        if (sample.psu_a_fault == "yes" && previous.psu_a_fault != "yes")
          ++out.psu_a_fault_events;
        if (sample.psu_b_fault == "yes" && previous.psu_b_fault != "yes")
          ++out.psu_b_fault_events;
        for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
        {
          if (sample.statuses[status] == "fault" &&
              previous.statuses[status] != "fault")
          {
            ++out.statuses[status].fault_events;
          }
        }
      }
      if (sample.uptime_s < previous.uptime_s)
        ++out.restart_events;
    }

    const double next_s = (i + 1 < aiors_samples.size())
        ? aiors_samples[i + 1].monotonic_s : now_s;
    const double observed_end_s = std::min(next_s,
        sample.monotonic_s + stale_after_s);
    const double segment_start_s = std::max(cutoff, sample.monotonic_s);
    const double segment_end_s = std::min(now_s, observed_end_s);
    if (segment_end_s <= segment_start_s)
      continue;

    const double duration_s = segment_end_s - segment_start_s;
    out.coverage_s += duration_s;
    if (sample.psu_a_state == "off")
      out.psu_a_off_s += duration_s;
    if (sample.psu_b_state == "off")
      out.psu_b_off_s += duration_s;
    if (sample.psu_a_fault == "yes")
      out.psu_a_fault_s += duration_s;
    if (sample.psu_b_fault == "yes")
      out.psu_b_fault_s += duration_s;
    for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
    {
      if (sample.statuses[status] == "fault")
        out.statuses[status].fault_s += duration_s;
    }
    if (sample.overall_status == "degraded")
      out.degraded_s += duration_s;
    else if (sample.overall_status == "fault")
      out.fault_s += duration_s;
  }

  for (double failure_s : aiors_failure_times)
  {
    if (failure_s >= cutoff)
      ++out.failure_samples;
  }
  return out;
}

void SvxStats::appendAiorsStats(std::ostringstream& os, double now_s) const
{
  const AiorsSample* current = aiors_samples.empty()
      ? nullptr : &aiors_samples.back();
  const AiorsWindow one_hour = aggregateAiorsWindow(now_s, 3600.0);
  const AiorsWindow one_day = aggregateAiorsWindow(now_s, 86400.0);

  auto age_s = [now_s](double timestamp) -> int64_t
  {
    if (timestamp <= 0.0)
      return -1;
    return static_cast<int64_t>(std::llround(
        std::max(0.0, now_s - timestamp)));
  };
  auto text_value = [current](const std::string AiorsSample::*member)
      -> std::string
  {
    return current ? stats_token(current->*member) : "na";
  };
  auto status_value = [current](AiorsStatus status) -> std::string
  {
    return current ? stats_token(current->statuses[status]) : "na";
  };
  auto seconds = [](double value) -> uint64_t
  {
    return static_cast<uint64_t>(std::llround(std::max(0.0, value)));
  };

  os << " aiors_enabled=" << (aiors_diagnostics_enabled ? 1 : 0)
     << " aiors_sample_ok=" << (last_aiors_attempt_ok ? 1 : 0)
     << " aiors_sample_error=" << stats_token(last_aiors_error)
     << " aiors_sample_age_s=" << age_s(last_aiors_success_monotonic)
     << " aiors_attempt_age_s=" << age_s(last_aiors_attempt_monotonic)
     << " aiors_sample_ts_unix_s=" << (current ? current->unix_s : 0)
     << " aiors_diagnostics_version="
     << (current ? std::to_string(current->diagnostics_version) : "na")
     << " aiors_software_version="
     << (current ? stats_token(current->software_version) : "na")
     << " aiors_software_revision="
     << (current ? stats_token(current->software_revision) : "na")
     << " aiors_hardware_version="
     << (current ? stats_token(current->hardware_version) : "na")
     << " aiors_uptime_s="
     << (current ? std::to_string(current->uptime_s) : "na")
     << " aiors_psu_current_scope="
     << (current && current->hardware_version == "1.1" ? "per_channel" :
         (current && current->hardware_version == "1.0" ? "common" : "unknown"))
     << " aiors_overall_status=" << text_value(&AiorsSample::overall_status)
     << " aiors_psu_gpio_status=" << text_value(&AiorsSample::psu_gpio_status)
     << " aiors_psu_a_state=" << text_value(&AiorsSample::psu_a_state)
     << " aiors_psu_a_fault=" << text_value(&AiorsSample::psu_a_fault)
     << " aiors_psu_b_state=" << text_value(&AiorsSample::psu_b_state)
     << " aiors_psu_b_fault=" << text_value(&AiorsSample::psu_b_fault)
     << " aiors_adc_status=" << text_value(&AiorsSample::adc_status)
     << " aiors_adc_primary_status="
     << text_value(&AiorsSample::adc_primary_status)
     << " aiors_adc_secondary_status="
     << text_value(&AiorsSample::adc_secondary_status)
     << " aiors_fan_state=" << text_value(&AiorsSample::fan_state)
     << " aiors_environment_status="
     << text_value(&AiorsSample::environment_status)
     << " aiors_digital_inputs_status="
     << text_value(&AiorsSample::digital_inputs_status)
     << " aiors_sa818_a_status=" << status_value(AIORS_STATUS_SA818_A)
     << " aiors_sa818_b_status=" << status_value(AIORS_STATUS_SA818_B)
     << " aiors_cm108_a_status=" << status_value(AIORS_STATUS_CM108_A)
     << " aiors_cm108_b_status=" << status_value(AIORS_STATUS_CM108_B);

  for (size_t i = 0; i < 4; ++i)
  {
    os << " aiors_di" << i << "="
       << (current ? stats_token(current->digital_inputs[i]) : "na");
  }

  os << " aiors_samples_ok_total=" << aiors_totals.success_samples
     << " aiors_samples_failed_total=" << aiors_totals.failure_samples
     << " aiors_state_coverage_s_total="
     << seconds(aiors_totals.state_coverage_s)
     << " aiors_energy_coverage_s_total="
     << seconds(aiors_totals.energy_coverage_s)
     << " aiors_restart_evt_total=" << aiors_totals.restart_events
     << " aiors_degraded_samples_total=" << aiors_totals.degraded_samples
     << " aiors_fault_samples_total=" << aiors_totals.fault_samples
     << " aiors_degraded_s_total=" << seconds(aiors_totals.degraded_s)
     << " aiors_fault_s_total=" << seconds(aiors_totals.fault_s)
     << " aiors_psu_a_state_change_evt_total="
     << aiors_totals.psu_a_state_changes
     << " aiors_psu_b_state_change_evt_total="
     << aiors_totals.psu_b_state_changes
     << " aiors_psu_a_on_s_total=" << seconds(aiors_totals.psu_a_on_s)
     << " aiors_psu_a_off_s_total=" << seconds(aiors_totals.psu_a_off_s)
     << " aiors_psu_b_on_s_total=" << seconds(aiors_totals.psu_b_on_s)
     << " aiors_psu_b_off_s_total=" << seconds(aiors_totals.psu_b_off_s)
     << " aiors_psu_a_fault_evt_total=" << aiors_totals.psu_a_fault_events
     << " aiors_psu_b_fault_evt_total=" << aiors_totals.psu_b_fault_events
     << " aiors_psu_a_fault_s_total=" << seconds(aiors_totals.psu_a_fault_s)
     << " aiors_psu_b_fault_s_total=" << seconds(aiors_totals.psu_b_fault_s)
     << " aiors_psu_a_energy_coverage_s_total="
     << seconds(aiors_totals.psu_a_energy_coverage_s)
     << " aiors_psu_b_energy_coverage_s_total="
     << seconds(aiors_totals.psu_b_energy_coverage_s);

  auto append_integrated_total = [&](const char* name, double value,
                                     double coverage_s)
  {
    os << " " << name << "=";
    if (coverage_s > 0.0)
      os << std::setprecision(6) << value;
    else
      os << "na";
  };
  append_integrated_total("aiors_psu_a_energy_wh_total",
      aiors_totals.psu_a_energy_wh,
      aiors_totals.psu_a_energy_coverage_s);
  append_integrated_total("aiors_psu_b_energy_wh_total",
      aiors_totals.psu_b_energy_wh,
      aiors_totals.psu_b_energy_coverage_s);
  append_integrated_total("aiors_psu_a_charge_ah_total",
      aiors_totals.psu_a_charge_ah,
      aiors_totals.psu_a_energy_coverage_s);
  append_integrated_total("aiors_psu_b_charge_ah_total",
      aiors_totals.psu_b_charge_ah,
      aiors_totals.psu_b_energy_coverage_s);

  os << " aiors_samples_ok_1h=" << one_hour.success_samples
     << " aiors_samples_failed_1h=" << one_hour.failure_samples
     << " aiors_state_coverage_s_1h=" << seconds(one_hour.coverage_s)
     << " aiors_restart_evt_1h=" << one_hour.restart_events
     << " aiors_degraded_samples_1h=" << one_hour.degraded_samples
     << " aiors_fault_samples_1h=" << one_hour.fault_samples
     << " aiors_degraded_s_1h=" << seconds(one_hour.degraded_s)
     << " aiors_fault_s_1h=" << seconds(one_hour.fault_s)
     << " aiors_psu_a_state_change_evt_1h=" << one_hour.psu_a_state_changes
     << " aiors_psu_b_state_change_evt_1h=" << one_hour.psu_b_state_changes
     << " aiors_psu_a_off_s_1h=" << seconds(one_hour.psu_a_off_s)
     << " aiors_psu_b_off_s_1h=" << seconds(one_hour.psu_b_off_s)
     << " aiors_psu_a_fault_evt_1h=" << one_hour.psu_a_fault_events
     << " aiors_psu_b_fault_evt_1h=" << one_hour.psu_b_fault_events
     << " aiors_psu_a_fault_s_1h=" << seconds(one_hour.psu_a_fault_s)
     << " aiors_psu_b_fault_s_1h=" << seconds(one_hour.psu_b_fault_s);

  static const char* status_names[AIORS_STATUS_COUNT] =
  {
    "aiors_sa818_a",
    "aiors_sa818_b",
    "aiors_cm108_a",
    "aiors_cm108_b"
  };
  auto append_status_window = [&](const AiorsWindow& window,
                                  const char* suffix)
  {
    for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
    {
      const StatusAgg& agg = window.statuses[status];
      os << " " << status_names[status] << "_fault_evt_" << suffix
         << "=" << agg.fault_events
         << " " << status_names[status] << "_fault_samples_" << suffix
         << "=" << agg.fault_samples
         << " " << status_names[status] << "_fault_s_" << suffix
         << "=" << seconds(agg.fault_s);
    }
  };
  append_status_window(one_hour, "1h");

  for (size_t status = 0; status < AIORS_STATUS_COUNT; ++status)
  {
    const StatusAgg& total = aiors_totals.statuses[status];
    os << " " << status_names[status] << "_fault_evt_total="
       << total.fault_events
       << " " << status_names[status] << "_fault_samples_total="
       << total.fault_samples
       << " " << status_names[status] << "_fault_s_total="
       << seconds(total.fault_s);
  }

  os << " aiors_samples_ok_24h=" << one_day.success_samples
     << " aiors_samples_failed_24h=" << one_day.failure_samples
     << " aiors_state_coverage_s_24h=" << seconds(one_day.coverage_s)
     << " aiors_restart_evt_24h=" << one_day.restart_events
     << " aiors_degraded_samples_24h=" << one_day.degraded_samples
     << " aiors_fault_samples_24h=" << one_day.fault_samples
     << " aiors_degraded_s_24h=" << seconds(one_day.degraded_s)
     << " aiors_fault_s_24h=" << seconds(one_day.fault_s)
     << " aiors_psu_a_state_change_evt_24h=" << one_day.psu_a_state_changes
     << " aiors_psu_b_state_change_evt_24h=" << one_day.psu_b_state_changes
     << " aiors_psu_a_off_s_24h=" << seconds(one_day.psu_a_off_s)
     << " aiors_psu_b_off_s_24h=" << seconds(one_day.psu_b_off_s)
     << " aiors_psu_a_fault_evt_24h=" << one_day.psu_a_fault_events
     << " aiors_psu_b_fault_evt_24h=" << one_day.psu_b_fault_events
     << " aiors_psu_a_fault_s_24h=" << seconds(one_day.psu_a_fault_s)
     << " aiors_psu_b_fault_s_24h=" << seconds(one_day.psu_b_fault_s);
  append_status_window(one_day, "24h");

  static const char* metric_names[AIORS_METRIC_COUNT] =
  {
    "aiors_psu_a_input_voltage_v",
    "aiors_psu_a_input_current_a",
    "aiors_psu_a_input_power_w",
    "aiors_psu_b_input_voltage_v",
    "aiors_psu_b_input_current_a",
    "aiors_psu_b_input_power_w",
    "aiors_bus_3v3_voltage_v",
    "aiors_bus_5v_voltage_v",
    "aiors_rf_a_average_voltage_v",
    "aiors_rf_b_average_voltage_v",
    "aiors_temperature_c",
    "aiors_humidity_rh",
    "aiors_ai0_voltage_v",
    "aiors_ai1_voltage_v",
    "aiors_ai2_voltage_v",
    "aiors_ai3_voltage_v"
  };

  os << std::setprecision(3);
  for (size_t metric = 0; metric < AIORS_METRIC_COUNT; ++metric)
  {
    const char* name = metric_names[metric];
    os << " " << name << "=";
    if (current && current->valid[metric])
      os << current->values[metric];
    else
      os << "na";

    const MetricAgg& hour_agg = one_hour.metrics[metric];
    os << " " << name << "_min_1h=";
    if (hour_agg.count) os << hour_agg.min; else os << "na";
    os << " " << name << "_max_1h=";
    if (hour_agg.count) os << hour_agg.max; else os << "na";
    os << " " << name << "_avg_1h=";
    if (hour_agg.count) os << (hour_agg.sum / hour_agg.count); else os << "na";
    os << " " << name << "_n_1h=" << hour_agg.count;

    const MetricAgg& day_agg = one_day.metrics[metric];
    os << " " << name << "_min_24h=";
    if (day_agg.count) os << day_agg.min; else os << "na";
    os << " " << name << "_max_24h=";
    if (day_agg.count) os << day_agg.max; else os << "na";
    os << " " << name << "_avg_24h=";
    if (day_agg.count) os << (day_agg.sum / day_agg.count); else os << "na";
    os << " " << name << "_n_24h=" << day_agg.count;
  }
}



void SvxStats::setPersistPath(const std::string& path)
{
  persist_path = path;
}

void SvxStats::setSnapshotPath(const std::string& path)
{
  snapshot_path = path;
}

void SvxStats::setHistoryPath(const std::string& path)
{
  history_path = path;
}

void SvxStats::setLogToStdout(bool enable)
{
  log_to_stdout = enable;
}

void SvxStats::setAiorsDiagnosticsEnabled(bool enable)
{
  aiors_diagnostics_enabled = enable;
}

void SvxStats::setAiorsctlPath(const std::string& path)
{
  aiorsctl_path = path;
}

void SvxStats::setAiorsSampleInterval(uint32_t interval_s)
{
  aiors_sample_interval_s = std::max<uint32_t>(5U,
      std::min<uint32_t>(3600U, interval_s));
}

void SvxStats::writeStatsReport(const std::string& line)
{
  if (!snapshot_path.empty())
  {
    const std::string tmp_path = snapshot_path + ".tmp";
    bool snapshot_written = false;
    {
      std::ofstream o(tmp_path.c_str(), std::ios::out | std::ios::trunc);
      if (o.is_open())
      {
        o << line << "\n";
        snapshot_written = o.good();
      }
    }

    if (snapshot_written &&
        std::rename(tmp_path.c_str(), snapshot_path.c_str()) != 0)
    {
      std::remove(snapshot_path.c_str());
      std::rename(tmp_path.c_str(), snapshot_path.c_str());
    }
  }

}

void SvxStats::statsWriterLoop()
{
  for (;;)
  {
    std::string snapshot;
    std::string persisted;
    std::deque<std::string> history;
    bool write_snapshot = false;
    bool write_persisted = false;
    {
      std::unique_lock<std::mutex> lock(stats_writer_mutex);
      stats_writer_ready.wait(lock, [this] {
        return stats_writer_stop || stats_snapshot_pending ||
               stats_persist_pending || !stats_history_pending.empty();
      });
      if (stats_writer_stop && !stats_snapshot_pending &&
          !stats_persist_pending && stats_history_pending.empty())
        return;
      write_snapshot = stats_snapshot_pending;
      write_persisted = stats_persist_pending;
      snapshot.swap(stats_snapshot_line);
      persisted.swap(stats_persist_text);
      history.swap(stats_history_pending);
      stats_snapshot_pending = false;
      stats_persist_pending = false;
    }
    if (write_snapshot)
      writeStatsReport(snapshot);
    if (!history.empty() && !history_path.empty())
    {
      std::ofstream o(history_path.c_str(), std::ios::out | std::ios::app);
      for (const auto& line : history)
        o << line << "\n";
    }
    if (write_persisted && !persist_path.empty())
    {
      const std::string tmp_path = persist_path + ".tmp";
      bool written = false;
      {
        std::ofstream o(tmp_path.c_str(), std::ios::out | std::ios::trunc);
        if (o.is_open())
        {
          o << persisted;
          written = o.good();
        }
      }
      if (written)
        std::rename(tmp_path.c_str(), persist_path.c_str());
    }
  }
}

void SvxStats::loadPersistedTotals()
{
  if (persist_path.empty())
    return;

  std::ifstream f(persist_path.c_str());
  if (!f.is_open())
    return;

  std::string line;
  while (std::getline(f, line))
  {
    if (line.empty())
      continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);

    auto parse_u64 = [&](uint64_t& dst) {
      try { dst = (uint64_t)std::stoull(v); } catch (...) {}
    };
    auto parse_d = [&](double& dst) {
      try { dst = std::stod(v); } catch (...) {}
    };

    // New (v4) persisted totals
    if (k == "rf_tx_s_total") parse_d(rf_tx_total.sec);
    else if (k == "rf_tx_evt_total") parse_u64(rf_tx_total.evt);
    else if (k == "rf_rx_s_total") parse_d(rf_rx_total.sec);
    else if (k == "rf_rx_evt_total") parse_u64(rf_rx_total.evt);
    else if (k == "sq_open_s_total") parse_d(sql_total.sec);
    else if (k == "sq_evt_total") parse_u64(sql_total.evt);
    else if (k == "frn_tx_s_total") parse_d(frn_tx_total.sec);
    else if (k == "frn_tx_evt_total") parse_u64(frn_tx_total.evt);
    else if (k == "frn_rx_s_total") parse_d(frn_rx_total.sec);
    else if (k == "frn_rx_evt_total") parse_u64(frn_rx_total.evt);
    else if (k == "cmd_ok_total") parse_u64(cmd_total.ok);
    else if (k == "cmd_rej_total") parse_u64(cmd_total.rej);
    else if (k == "cmd_authfail_total") parse_u64(cmd_total.authfail);
    else if (k == "cmd_bc_attempt_total") parse_u64(cmd_total.bc_attempt);
    else if (k == "cmd_sum_ms_total") parse_u64(cmd_total.sum_ms);
    else if (k == "cmd_max_ms_total") parse_u64(cmd_total.max_ms);
    else if (k == "frn_tx_bytes_total") parse_u64(frn_tx_bytes_total);
    else if (k == "frn_rx_bytes_total") parse_u64(frn_rx_bytes_total);
    else if (k == "frn_link_up_total") parse_u64(frn_link_up_total);
    else if (k == "frn_link_down_total") parse_u64(frn_link_down_total);
    else if (k == "aiors_samples_ok_total") parse_u64(aiors_totals.success_samples);
    else if (k == "aiors_samples_failed_total") parse_u64(aiors_totals.failure_samples);
    else if (k == "aiors_restart_evt_total") parse_u64(aiors_totals.restart_events);
    else if (k == "aiors_degraded_samples_total") parse_u64(aiors_totals.degraded_samples);
    else if (k == "aiors_fault_samples_total") parse_u64(aiors_totals.fault_samples);
    else if (k == "aiors_psu_a_state_change_evt_total") parse_u64(aiors_totals.psu_a_state_changes);
    else if (k == "aiors_psu_b_state_change_evt_total") parse_u64(aiors_totals.psu_b_state_changes);
    else if (k == "aiors_psu_a_fault_evt_total") parse_u64(aiors_totals.psu_a_fault_events);
    else if (k == "aiors_psu_b_fault_evt_total") parse_u64(aiors_totals.psu_b_fault_events);
    else if (k == "aiors_state_coverage_s_total") parse_d(aiors_totals.state_coverage_s);
    else if (k == "aiors_energy_coverage_s_total") parse_d(aiors_totals.energy_coverage_s);
    else if (k == "aiors_psu_a_energy_coverage_s_total") parse_d(aiors_totals.psu_a_energy_coverage_s);
    else if (k == "aiors_psu_b_energy_coverage_s_total") parse_d(aiors_totals.psu_b_energy_coverage_s);
    else if (k == "aiors_psu_a_energy_wh_total") parse_d(aiors_totals.psu_a_energy_wh);
    else if (k == "aiors_psu_b_energy_wh_total") parse_d(aiors_totals.psu_b_energy_wh);
    else if (k == "aiors_psu_a_charge_ah_total") parse_d(aiors_totals.psu_a_charge_ah);
    else if (k == "aiors_psu_b_charge_ah_total") parse_d(aiors_totals.psu_b_charge_ah);
    else if (k == "aiors_psu_a_on_s_total") parse_d(aiors_totals.psu_a_on_s);
    else if (k == "aiors_psu_a_off_s_total") parse_d(aiors_totals.psu_a_off_s);
    else if (k == "aiors_psu_b_on_s_total") parse_d(aiors_totals.psu_b_on_s);
    else if (k == "aiors_psu_b_off_s_total") parse_d(aiors_totals.psu_b_off_s);
    else if (k == "aiors_psu_a_fault_s_total") parse_d(aiors_totals.psu_a_fault_s);
    else if (k == "aiors_psu_b_fault_s_total") parse_d(aiors_totals.psu_b_fault_s);
    else if (k == "aiors_degraded_s_total") parse_d(aiors_totals.degraded_s);
    else if (k == "aiors_fault_s_total") parse_d(aiors_totals.fault_s);
    else if (k == "aiors_sa818_a_fault_evt_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_SA818_A].fault_events);
    else if (k == "aiors_sa818_a_fault_samples_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_SA818_A].fault_samples);
    else if (k == "aiors_sa818_a_fault_s_total") parse_d(aiors_totals.statuses[AIORS_STATUS_SA818_A].fault_s);
    else if (k == "aiors_sa818_b_fault_evt_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_SA818_B].fault_events);
    else if (k == "aiors_sa818_b_fault_samples_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_SA818_B].fault_samples);
    else if (k == "aiors_sa818_b_fault_s_total") parse_d(aiors_totals.statuses[AIORS_STATUS_SA818_B].fault_s);
    else if (k == "aiors_cm108_a_fault_evt_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_CM108_A].fault_events);
    else if (k == "aiors_cm108_a_fault_samples_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_CM108_A].fault_samples);
    else if (k == "aiors_cm108_a_fault_s_total") parse_d(aiors_totals.statuses[AIORS_STATUS_CM108_A].fault_s);
    else if (k == "aiors_cm108_b_fault_evt_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_CM108_B].fault_events);
    else if (k == "aiors_cm108_b_fault_samples_total") parse_u64(aiors_totals.statuses[AIORS_STATUS_CM108_B].fault_samples);
    else if (k == "aiors_cm108_b_fault_s_total") parse_d(aiors_totals.statuses[AIORS_STATUS_CM108_B].fault_s);

    // Legacy keys (older experiments)
    else if (k == "rf_tx_hours_total") { double h=0; parse_d(h); rf_tx_total.sec = h * 3600.0; }
    else if (k == "rf_tx_total") { uint64_t v0=0; parse_u64(v0); rf_tx_total.evt = v0; }
    else if (k == "sql_total") { uint64_t v0=0; parse_u64(v0); sql_total.evt = v0; }
    else if (k == "cmd_total") { uint64_t v0=0; parse_u64(v0); cmd_total.ok = v0; }
    else if (k == "frn_tx_total") { uint64_t v0=0; parse_u64(v0); frn_tx_total.evt = v0; }
    else if (k == "frn_rx_total") { uint64_t v0=0; parse_u64(v0); frn_rx_total.evt = v0; }
  }
}

void SvxStats::savePersistedTotals()
{
  if (persist_path.empty())
    return;

  std::ostringstream o;

  o << std::setprecision(17);
  o << "rf_tx_s_total=" << rf_tx_total.sec << "\n";
  o << "rf_tx_evt_total=" << rf_tx_total.evt << "\n";
  o << "rf_rx_s_total=" << rf_rx_total.sec << "\n";
  o << "rf_rx_evt_total=" << rf_rx_total.evt << "\n";
  o << "sq_open_s_total=" << sql_total.sec << "\n";
  o << "sq_evt_total=" << sql_total.evt << "\n";
  o << "frn_tx_s_total=" << frn_tx_total.sec << "\n";
  o << "frn_tx_evt_total=" << frn_tx_total.evt << "\n";
  o << "frn_rx_s_total=" << frn_rx_total.sec << "\n";
  o << "frn_rx_evt_total=" << frn_rx_total.evt << "\n";
  o << "cmd_ok_total=" << cmd_total.ok << "\n";
  o << "cmd_rej_total=" << cmd_total.rej << "\n";
  o << "cmd_authfail_total=" << cmd_total.authfail << "\n";
  o << "cmd_bc_attempt_total=" << cmd_total.bc_attempt << "\n";
  o << "cmd_sum_ms_total=" << cmd_total.sum_ms << "\n";
  o << "cmd_max_ms_total=" << cmd_total.max_ms << "\n";
  o << "frn_tx_bytes_total=" << frn_tx_bytes_total << "\n";
  o << "frn_rx_bytes_total=" << frn_rx_bytes_total << "\n";
  o << "frn_link_up_total=" << frn_link_up_total << "\n";
  o << "frn_link_down_total=" << frn_link_down_total << "\n";
  o << "aiors_samples_ok_total=" << aiors_totals.success_samples << "\n";
  o << "aiors_samples_failed_total=" << aiors_totals.failure_samples << "\n";
  o << "aiors_restart_evt_total=" << aiors_totals.restart_events << "\n";
  o << "aiors_degraded_samples_total=" << aiors_totals.degraded_samples << "\n";
  o << "aiors_fault_samples_total=" << aiors_totals.fault_samples << "\n";
  o << "aiors_psu_a_state_change_evt_total=" << aiors_totals.psu_a_state_changes << "\n";
  o << "aiors_psu_b_state_change_evt_total=" << aiors_totals.psu_b_state_changes << "\n";
  o << "aiors_psu_a_fault_evt_total=" << aiors_totals.psu_a_fault_events << "\n";
  o << "aiors_psu_b_fault_evt_total=" << aiors_totals.psu_b_fault_events << "\n";
  o << "aiors_state_coverage_s_total=" << aiors_totals.state_coverage_s << "\n";
  o << "aiors_energy_coverage_s_total=" << aiors_totals.energy_coverage_s << "\n";
  o << "aiors_psu_a_energy_coverage_s_total=" << aiors_totals.psu_a_energy_coverage_s << "\n";
  o << "aiors_psu_b_energy_coverage_s_total=" << aiors_totals.psu_b_energy_coverage_s << "\n";
  o << "aiors_psu_a_energy_wh_total=" << aiors_totals.psu_a_energy_wh << "\n";
  o << "aiors_psu_b_energy_wh_total=" << aiors_totals.psu_b_energy_wh << "\n";
  o << "aiors_psu_a_charge_ah_total=" << aiors_totals.psu_a_charge_ah << "\n";
  o << "aiors_psu_b_charge_ah_total=" << aiors_totals.psu_b_charge_ah << "\n";
  o << "aiors_psu_a_on_s_total=" << aiors_totals.psu_a_on_s << "\n";
  o << "aiors_psu_a_off_s_total=" << aiors_totals.psu_a_off_s << "\n";
  o << "aiors_psu_b_on_s_total=" << aiors_totals.psu_b_on_s << "\n";
  o << "aiors_psu_b_off_s_total=" << aiors_totals.psu_b_off_s << "\n";
  o << "aiors_psu_a_fault_s_total=" << aiors_totals.psu_a_fault_s << "\n";
  o << "aiors_psu_b_fault_s_total=" << aiors_totals.psu_b_fault_s << "\n";
  o << "aiors_degraded_s_total=" << aiors_totals.degraded_s << "\n";
  o << "aiors_fault_s_total=" << aiors_totals.fault_s << "\n";
  o << "aiors_sa818_a_fault_evt_total=" << aiors_totals.statuses[AIORS_STATUS_SA818_A].fault_events << "\n";
  o << "aiors_sa818_a_fault_samples_total=" << aiors_totals.statuses[AIORS_STATUS_SA818_A].fault_samples << "\n";
  o << "aiors_sa818_a_fault_s_total=" << aiors_totals.statuses[AIORS_STATUS_SA818_A].fault_s << "\n";
  o << "aiors_sa818_b_fault_evt_total=" << aiors_totals.statuses[AIORS_STATUS_SA818_B].fault_events << "\n";
  o << "aiors_sa818_b_fault_samples_total=" << aiors_totals.statuses[AIORS_STATUS_SA818_B].fault_samples << "\n";
  o << "aiors_sa818_b_fault_s_total=" << aiors_totals.statuses[AIORS_STATUS_SA818_B].fault_s << "\n";
  o << "aiors_cm108_a_fault_evt_total=" << aiors_totals.statuses[AIORS_STATUS_CM108_A].fault_events << "\n";
  o << "aiors_cm108_a_fault_samples_total=" << aiors_totals.statuses[AIORS_STATUS_CM108_A].fault_samples << "\n";
  o << "aiors_cm108_a_fault_s_total=" << aiors_totals.statuses[AIORS_STATUS_CM108_A].fault_s << "\n";
  o << "aiors_cm108_b_fault_evt_total=" << aiors_totals.statuses[AIORS_STATUS_CM108_B].fault_events << "\n";
  o << "aiors_cm108_b_fault_samples_total=" << aiors_totals.statuses[AIORS_STATUS_CM108_B].fault_samples << "\n";
  o << "aiors_cm108_b_fault_s_total=" << aiors_totals.statuses[AIORS_STATUS_CM108_B].fault_s << "\n";
  {
    std::lock_guard<std::mutex> lock(stats_writer_mutex);
    stats_persist_text = o.str();
    stats_persist_pending = true;
  }
  stats_writer_ready.notify_one();
}
