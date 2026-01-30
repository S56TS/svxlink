#include "SvxStats.h"

#include <AsyncTimer.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>

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

SvxStats& SvxStats::instance()
{
  static SvxStats inst;
  return inst;
}

SvxStats::SvxStats()
{
  start_monotonic = mono_now_sec();
  cur_minute = minute_now();
  // initialize buckets
  for (size_t i = 0; i < NBUCKET; ++i)
  {
    buckets[i].minute = cur_minute;
  }
  idx = 0;
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

  stats_interval_ms = (interval_s == 0 ? 60000U : (uint32_t)(interval_s * 1000U));
  tick_timer = new Async::Timer(stats_interval_ms);
  tick_timer->setEnable(true);
  tick_timer->expired.connect(sigc::mem_fun(*this, &SvxStats::onTick));
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
  buckets[idx].frn_tx_bytes += bytes;
  frn_tx_bytes_total += bytes;
}

void SvxStats::addFrnRxBytes(uint64_t bytes)
{
  uint64_t m = minute_now();
  rotateBuckets(m);
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
  DurAgg frn_tx_1h, frn_rx_1h, rf_tx_1h, rf_rx_1h, sql_1h;
  CmdAgg cmd_1h;
  uint64_t frn_tx_bytes_1h = 0, frn_rx_bytes_1h = 0;
  uint64_t join_1h = 0, leave_1h = 0;

  // Compute peak users in last hour as max of sampled current size at updates is not stored.
  // We'll approximate by taking max current users seen via client list updates in window.
  // Since we don't store historical "users_cur", we compute peak as max of current within hour updates
  // by scanning user_last_seen_minute map size per minute is expensive, so we instead track
  // peak by updating onTick.
  (void)join_1h;

  for (size_t i = 0; i < NBUCKET; ++i)
  {
    const Bucket& b = buckets[i];
    // only include last 60 minutes, including current
    if (b.minute + NBUCKET <= now_m) continue;
    duragg_merge(frn_tx_1h, b.frn_tx);
    duragg_merge(frn_rx_1h, b.frn_rx);
    duragg_merge(rf_tx_1h, b.rf_tx);
    duragg_merge(rf_rx_1h, b.rf_rx);
    duragg_merge(sql_1h, b.sql);
    cmdagg_merge(cmd_1h, b.cmd);
    frn_tx_bytes_1h += b.frn_tx_bytes;
    frn_rx_bytes_1h += b.frn_rx_bytes;
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
  DurAgg sql_total_eff    = sql_total;

  add_active(frn_tx_active, frn_tx_t0, frn_tx_1h, frn_tx_total_eff);
  add_active(frn_rx_active, frn_rx_t0, frn_rx_1h, frn_rx_total_eff);
  add_active(rf_tx_active,  rf_tx_t0,  rf_tx_1h,  rf_tx_total_eff);
  add_active(rf_rx_active,  rf_rx_t0,  rf_rx_1h,  rf_rx_total_eff);
  add_active(sql_active,    sql_t0,    sql_1h,    sql_total_eff);

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

  std::ostringstream os;
  os.setf(std::ios::fixed);
  os << "STATS"
     << " uptime_s=" << uptimeSeconds()
     << " frn_users=" << frn_users_cur
     << " frn_users_peak_1h=" << peak_1h
     << " frn_users_unique_1h=" << unique_1h
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

     << " sq_evt_1h=" << sql_1h.evt
     << " sq_open_s_1h=" << (uint64_t)std::llround(sql_1h.sec)
     << " sq_min_s_1h=" << (sql_1h.min_sec > 0.0 ? sql_1h.min_sec : 0.0)
     << " sq_max_s_1h=" << sql_1h.max_sec

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

  
  return os.str();
}

std::vector<std::string> SvxStats::formatStatsGroups()
{
  uint64_t now_m = minute_now();
  rotateBuckets(now_m);
  double now_s = mono_now_sec();

  DurAgg frn_tx_1h, frn_rx_1h, rf_tx_1h, rf_rx_1h, sql_1h;
  CmdAgg cmd_1h;
  uint64_t frn_tx_bytes_1h = 0, frn_rx_bytes_1h = 0;
  uint64_t join_1h = 0, leave_1h = 0;

  for (size_t i = 0; i < NBUCKET; ++i)
  {
    const Bucket& b = buckets[i];
    if (b.minute + NBUCKET <= now_m) continue;
    duragg_merge(frn_tx_1h, b.frn_tx);
    duragg_merge(frn_rx_1h, b.frn_rx);
    duragg_merge(rf_tx_1h, b.rf_tx);
    duragg_merge(rf_rx_1h, b.rf_rx);
    duragg_merge(sql_1h, b.sql);
    cmdagg_merge(cmd_1h, b.cmd);
    frn_tx_bytes_1h += b.frn_tx_bytes;
    frn_rx_bytes_1h += b.frn_rx_bytes;
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
  DurAgg sql_total_eff    = sql_total;

  add_active(frn_tx_active, frn_tx_t0, frn_tx_1h, frn_tx_total_eff);
  add_active(frn_rx_active, frn_rx_t0, frn_rx_1h, frn_rx_total_eff);
  add_active(rf_tx_active,  rf_tx_t0,  rf_tx_1h,  rf_tx_total_eff);
  add_active(rf_rx_active,  rf_rx_t0,  rf_rx_1h,  rf_rx_total_eff);
  add_active(sql_active,    sql_t0,    sql_1h,    sql_total_eff);

  uint64_t unique_1h = 0;
  for (const auto& kv : user_last_seen_minute)
  {
    if (kv.second + NBUCKET > now_m) unique_1h++;
  }
  uint64_t peak_1h = frn_users_peak_1h;

  auto pct = [](double sec) -> double { return std::min(100.0, std::max(0.0, sec * 100.0 / 3600.0)); };
  auto sec_u64 = [](double sec) -> uint64_t { return (uint64_t)std::llround(std::max(0.0, sec)); };

  std::vector<std::string> out;
  out.reserve(8);

  // General / users
  {
    std::ostringstream os;
    os << "STATS uptime=" << uptimeSeconds() << "s"
       << " users=" << frn_users_cur
       << " peak1h=" << peak_1h
       << " uniq1h=" << unique_1h
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

  std::cout << formatStatsLine() << std::endl;  savePersistedTotals();
}



void SvxStats::setPersistPath(const std::string& path)
{
  persist_path = path;
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

  std::ofstream o(persist_path.c_str(), std::ios::out | std::ios::trunc);
  if (!o.is_open())
    return;

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
}
