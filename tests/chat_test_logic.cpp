// Real LinkManager integration fixture. No radios or network modules are used.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>
#include <AsyncAudioSource.h>
#include <AsyncAudioSink.h>
#include <AsyncTimer.h>
#include "LogicBase.h"

class ChatTestLogic : public LogicBase, private Async::AudioSource,
                      private Async::AudioSink
{
public:
  ChatTestLogic() : timer(20, Async::Timer::TYPE_PERIODIC, false) {}
  bool initialize(Async::Config& config, const std::string& name) override
  {
    std::string input, output;
    config.getValue(name, "INPUT_FILE", input);
    config.getValue(name, "OUTPUT_FILE", output);
    config.getValue(name, "SECOND_MS", second_ms);
    config.getValue(name, "START_MS", start_ms);
    config.getValue(name, "STOP_ON_PLAY", stop_on_play);
    std::ifstream file(input, std::ios::binary);
    int16_t value;
    while (file.read(reinterpret_cast<char*>(&value), sizeof(value)))
      samples.push_back(value / 32768.0f);
    recording.open(output, std::ios::binary);
    if (samples.empty() || !recording || !LogicBase::initialize(config, name)) return false;
    timer.expired.connect([this](Async::Timer*) { tick(); });
    timer.setEnable(true);
    return true;
  }
  Async::AudioSink* logicConIn() override { return this; }
  Async::AudioSource* logicConOut() override { return this; }
  ~ChatTestLogic() override
  {
    timer.setEnable(false);
    if (LinkManager::hasInstance()) LinkManager::instance()->deleteLogic(this);
  }
private:
  Async::Timer timer;
  std::vector<float> samples;
  std::ofstream recording;
  unsigned elapsed = 0, second_ms = 0, start_ms = 500;
  size_t position = 0;
  bool sending = false;
  bool stop_on_play = false;
  void tick()
  {
    elapsed += 20;
    if (elapsed == start_ms || (second_ms && elapsed == second_ms))
    {
      std::cout << "TEST fixture " << name() << " start " << elapsed << std::endl;
      sending = true; position = 0; setIdle(false);
    }
    if (!sending) return;
    int n = std::min<size_t>(INTERNAL_SAMPLE_RATE/50, samples.size()-position);
    int accepted = sinkWriteSamples(samples.data()+position, n);
    if (accepted > 0) position += accepted;
    if (position == samples.size())
    {
      std::cout << "TEST fixture " << name() << " sent " << position << std::endl;
      sending = false; sinkFlushSamples();
    }
  }
  int writeSamples(const float* audio, int count) override
  {
    recording.write(reinterpret_cast<const char*>(audio), count*sizeof(float));
    recording.flush();
    if (stop_on_play)
    {
      stop_on_play = false;
      publishStateEvent("Reflector:tg_selected", "42");
    }
    return count;
  }
  void flushSamples() override { sourceAllSamplesFlushed(); }
  void resumeOutput() override {}
  void allSamplesFlushed() override { setIdle(true); }
};
extern "C" LogicBase* construct() { return new ChatTestLogic; }
