/**
 * Optional voice-service logic. Audio stays on the SvxLink event loop; cloud
 * requests and wake-word inference run in the separate svxlink-chat service.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <json/json.h>
#include <AsyncAudioSink.h>
#include <AsyncAudioSource.h>
#include <AsyncConfig.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTcpClient.h>
#include <AsyncTimer.h>
#include "Logic.h"
#include "Module.h"
#include "Rx.h"
#include "SvxSwDtmfDecoder.h"

namespace {
using Clock = std::chrono::steady_clock;
using Connection = Async::TcpClient<Async::FramedTcpConnection>;

class ChatLogic : public LogicBase, private Async::AudioSink,
                  private Async::AudioSource
{
public:
  ChatLogic() : m_tick(20, Async::Timer::TYPE_PERIODIC, false),
                m_end(200, Async::Timer::TYPE_ONESHOT, false) {}

  bool initialize(Async::Config& cfg, const std::string& name) override
  {
    unsigned port = 5301;
    cfg.getValue(name, "SERVICE_PORT", port);
    cfg.getValue(name, "TX_DELAY_MS", m_tx_delay);
    cfg.getValue(name, "MAX_REPLY_SECONDS", m_max_reply);
    cfg.getValue(name, "DTMF_START", m_start);
    cfg.getValue(name, "DTMF_STOP", m_stop);
    cfg.getValue(name, "DTMF_RESET", m_reset);
    cfg.getValue(name, "NETWORK_DTMF", m_network_dtmf);
    if (port == 0 || port > 65535 || m_tx_delay > 2000 ||
        m_max_reply < 1 || m_max_reply > 60 ||
        !validCommand(m_start) || !validCommand(m_stop) ||
        !validCommand(m_reset) || std::set<std::string>{m_start,m_stop,m_reset}.size()!=3)
    {
      std::cerr << name << ": Invalid ChatLogic configuration\n";
      return false;
    }
    if (!LogicBase::initialize(cfg, name)) return false;
    m_con.reset(new Connection("127.0.0.1", port));
    m_con->setMaxRxFrameSize(5 + INTERNAL_SAMPLE_RATE * 2 * m_max_reply);
    m_con->setMaxTxFrameSize(65536);
    m_con->connected.connect([this]() {
      m_unacked = 0;
      m_last_peer = Clock::now();
      ++m_epoch;
      Json::Value msg;
      msg["event"] = "hello";
      msg["version"] = 1;
      msg["sample_rate"] = INTERNAL_SAMPLE_RATE;
      msg["max_reply_seconds"] = m_max_reply;
      send(msg);
      std::cout << this->name() << ": Voice service connected\n";
    });
    m_con->disconnected.connect([this](Async::FramedTcpConnection*,
                                      Async::TcpConnection::DisconnectReason) {
      lostConnection();
    });
    m_con->frameReceived.connect([this](Async::FramedTcpConnection*,
                                       std::vector<uint8_t>& frame) { received(frame); });
    m_tick.expired.connect([this](Async::Timer*) { tick(); });
    m_end.expired.connect([this](Async::Timer*) { endInput(); });
    m_tick.setEnable(true);
    m_con->connect();
    return true;
  }

  Async::AudioSink* logicConIn() override { return this; }
  Async::AudioSource* logicConOut() override { return this; }

  void remoteReceivedPublishStateEvent(LogicBase*, const std::string& event,
                                       const std::string& msg) override
  {
    if (event == "Reflector:tg_selected") { command("stop"); return; }
    if (event != "Chat:command") return;
    const std::string prefix = name() + " ";
    if (msg.compare(0, prefix.size(), prefix) != 0) return;
    const auto action = msg.substr(prefix.size());
    if (action != "start" && action != "stop" && action != "reset") return;
    if (m_receiving) m_pending_command = action;
    else command(action);
  }

  void remoteReceivedTgUpdated(LogicBase* logic, uint32_t tg) override
  {
    auto previous = m_tgs.find(logic->name());
    if (previous != m_tgs.end() && previous->second != tg)
      command("stop");
    m_tgs[logic->name()] = tg;
  }

  ~ChatLogic() override
  {
    m_tick.setEnable(false);
    m_end.setEnable(false);
    if (m_con) m_con->disconnect();
    // Unregister while the AudioSink/AudioSource bases are still alive.
    if (LinkManager::hasInstance()) LinkManager::instance()->deleteLogic(this);
  }

private:
  std::unique_ptr<Connection> m_con;
  std::unique_ptr<SvxSwDtmfDecoder> m_dtmf;
  Async::Timer m_tick, m_end;
  std::vector<float> m_output;
  size_t m_position = 0, m_unacked = 0;
  uint32_t m_epoch = 0;
  unsigned m_tx_delay = 250, m_max_reply = 30, m_ticks = 0, m_stalled = 0;
  bool m_receiving = false, m_flushing = false, m_network_dtmf = true;
  bool m_report_played = false, m_writing = false;
  std::string m_source, m_digits, m_pending_command, m_deferred_command;
  std::string m_start = "981", m_stop = "980", m_reset = "982";
  std::map<std::string, uint32_t> m_tgs;
  Clock::time_point m_last_peer = Clock::now(), m_last_digit = Clock::now();

  static bool validCommand(const std::string& cmd)
  {
    return !cmd.empty() && cmd.size() <= 16 &&
        cmd.find_first_not_of("0123456789ABCD") == std::string::npos;
  }

  void send(Json::Value msg)
  {
    if (!m_con || !m_con->isConnected()) return;
    msg["epoch"] = m_epoch;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    auto text = "J" + Json::writeString(builder, msg);
    if (m_con->write(text.data(), text.size()) < 0) lostConnection();
  }

  void event(const char* value)
  {
    Json::Value msg;
    msg["event"] = value;
    send(msg);
  }

  void clearOutput()
  {
    m_report_played = false;
    bool had_output = !m_output.empty();
    m_output.clear();
    m_position = 0;
    m_stalled = 0;
    if (had_output && !m_flushing)
    {
      m_flushing = true;
      sinkFlushSamples();
    }
    setIdle(!m_receiving && !m_flushing);
  }

  void lostConnection()
  {
    if (m_con) m_con->disconnect();
    ++m_epoch;
    m_unacked = 0;
    clearOutput();
  }

  void command(const std::string& action)
  {
    // A downstream logic can publish a TG change synchronously from its
    // writeSamples callback. Keep the current buffer alive until it returns.
    if (m_writing) { m_deferred_command = action; return; }
    ++m_epoch;
    clearOutput();
    Json::Value msg;
    msg["event"] = "command";
    msg["action"] = action;
    send(msg);
    if (!m_con->isConnected())
      std::cerr << name() << ": Voice service unavailable (" << action << ")\n";
  }

  void digit(char value, int)
  {
    if (Clock::now() - m_last_digit > std::chrono::seconds(3)) m_digits.clear();
    m_last_digit = Clock::now();
    if (value == '#')
    {
      if (m_digits == m_start) m_pending_command = "start";
      else if (m_digits == m_stop) m_pending_command = "stop";
      else if (m_digits == m_reset) m_pending_command = "reset";
      m_digits.clear();
    }
    else if (m_digits.size() < 32) m_digits += value;
    else m_digits = "!"; // An overlong command must never become a valid suffix.
  }

  int writeSamples(const float* samples, int count) override
  {
    if (count <= 0) return count;
    auto talker = LinkManager::instance()->currentTalkerFor(name());
    if (!talker) return count;
    auto radio = dynamic_cast<Logic*>(talker);
    bool rf = radio && radio->rx().squelchIsOpen();
    if (radio && !rf && (!radio->activeModule() ||
                        radio->activeModule()->name() != "Frn")) return count;
    std::string source = talker->name() + (rf ? ":rf" : radio ? ":frn" : ":link");
    if (m_receiving && source != m_source) endInput();
    if (!m_receiving)
    {
      ++m_epoch;
      clearOutput();
      m_receiving = true;
      m_source = source;
      m_digits.clear();
      m_pending_command.clear();
      m_dtmf.reset();
      if (!rf && m_network_dtmf)
      {
        m_dtmf.reset(new SvxSwDtmfDecoder(cfg(), name()));
        if (!m_dtmf->initialize()) m_dtmf.reset();
        else m_dtmf->digitDeactivated.connect(sigc::mem_fun(*this, &ChatLogic::digit));
      }
      Json::Value msg;
      msg["event"] = "begin";
      msg["source"] = source;
      msg["talkgroup"] = talker->receivedTg();
      send(msg);
      setIdle(false);
    }
    m_end.setEnable(false);
    if (m_dtmf) m_dtmf->writeSamples(samples, count);
    if (!m_con->isConnected()) return count;
    // Bounded in-flight audio: a stalled helper must never stall RF or consume
    // unbounded memory in FramedTcpConnection's transmit queue.
    if (m_unacked + 2 * count > 4 * INTERNAL_SAMPLE_RATE)
    {
      lostConnection();
      return count;
    }
    for (int offset = 0; offset < count; offset += 2048)
    {
      int n = std::min(2048, count - offset);
      std::vector<uint8_t> frame(1 + 2*n);
      frame[0] = 'A';
      for (int i = 0; i < n; ++i)
      {
        float value = samples[offset+i];
        if (!std::isfinite(value)) value = 0;
        int16_t pcm = std::lround(std::max(-1.0f, std::min(1.0f, value))*32767);
        frame[1+2*i] = pcm & 0xff;
        frame[2+2*i] = (static_cast<uint16_t>(pcm) >> 8);
      }
      m_unacked += 2*n;
      if (m_con->write(frame.data(), frame.size()) < 0) { lostConnection(); break; }
    }
    return count;
  }

  void flushSamples() override
  {
    if (m_receiving) { m_end.setEnable(true); m_end.reset(); }
    sourceAllSamplesFlushed();
  }

  void endInput()
  {
    m_end.setEnable(false);
    if (!m_receiving) return;
    if (m_dtmf)
    {
      std::vector<float> silence(INTERNAL_SAMPLE_RATE / 10, 0);
      m_dtmf->writeSamples(silence.data(), silence.size());
      m_dtmf.reset();
    }
    m_receiving = false;
    if (!m_pending_command.empty())
    {
      command(m_pending_command);
      m_pending_command.clear();
    }
    else event("end");
    setIdle(m_output.empty() && !m_flushing);
  }

  void received(std::vector<uint8_t>& frame)
  {
    if (frame.empty()) { lostConnection(); return; }
    m_last_peer = Clock::now();
    if (frame[0] == 'J')
    {
      Json::Value msg;
      Json::CharReaderBuilder builder;
      std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
      const char* begin = reinterpret_cast<const char*>(frame.data()+1);
      std::string errors;
      if (!reader->parse(begin, begin+frame.size()-1, &msg, &errors) || !msg.isObject())
      { lostConnection(); return; }
      if (!msg["event"].isString()) { lostConnection(); return; }
      if (msg["event"].asString() == "ack" && msg["bytes"].isUInt())
        m_unacked -= std::min(m_unacked, static_cast<size_t>(msg["bytes"].asUInt()));
      return;
    }
    if (frame[0] != 'P' || frame.size() < 7 || (frame.size()-5)%2)
    { lostConnection(); return; }
    uint32_t epoch = (uint32_t(frame[1])<<24) | (uint32_t(frame[2])<<16) |
                     (uint32_t(frame[3])<<8) | frame[4];
    if (epoch != m_epoch) return;
    if (m_receiving || m_flushing || !m_output.empty())
    {
      // Release the helper's pending response even if a downstream sink is
      // still draining an interrupted transmission.
      event("interrupted");
      return;
    }
    size_t count = (frame.size()-5)/2;
    if (count > INTERNAL_SAMPLE_RATE*m_max_reply) { lostConnection(); return; }
    m_output.assign(INTERNAL_SAMPLE_RATE*m_tx_delay/1000, 0.0f);
    for (size_t i=0; i<count; ++i)
    {
      int16_t pcm = static_cast<int16_t>(uint16_t(frame[5+2*i]) |
                                       (uint16_t(frame[6+2*i])<<8));
      m_output.push_back(pcm/32768.0f);
    }
    m_position = 0;
    setIdle(false);
  }

  void tick()
  {
    if (++m_ticks % 50 == 0)
    {
      if (m_con->isConnected())
      {
        if (Clock::now()-m_last_peer > std::chrono::seconds(5)) lostConnection();
        else event("ping");
      }
      else if (m_con->isIdle() && m_ticks % 150 == 0) m_con->connect();
    }
    if (m_output.empty() || m_receiving || m_flushing) return;
    int n = std::min<size_t>(INTERNAL_SAMPLE_RATE/50, m_output.size()-m_position);
    m_writing = true;
    int written = sinkWriteSamples(m_output.data()+m_position, n);
    m_writing = false;
    if (!m_deferred_command.empty())
    {
      const auto action = m_deferred_command;
      m_deferred_command.clear();
      command(action);
      return;
    }
    if (written <= 0)
    {
      if (++m_stalled > 100) { ++m_epoch; clearOutput(); event("interrupted"); }
      return;
    }
    m_stalled = 0;
    m_position += written;
    if (m_position == m_output.size())
    {
      m_output.clear();
      m_position = 0;
      m_flushing = true;
      m_report_played = true;
      sinkFlushSamples();
    }
  }

  void resumeOutput() override {} // Timer paces output, including after backpressure.
  void allSamplesFlushed() override
  {
    m_flushing = false;
    if (m_report_played) event("played");
    m_report_played = false;
    setIdle(!m_receiving && m_output.empty());
  }
};
} // namespace

extern "C" LogicBase* construct() { return new ChatLogic; }
