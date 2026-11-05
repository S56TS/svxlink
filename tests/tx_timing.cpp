// Real PttCtrl timers/AudioFifo, with a sink that applies backpressure.
#include <bits/stdc++.h>
#include <AsyncCppApplication.h>
#include <AsyncAudioDecoderS16.h>
#include <AsyncAudioSink.h>
#include <AsyncTimer.h>
#include "PttCtrl.h"

using Clock = std::chrono::steady_clock;
struct TimedSink : Async::AudioSink
{
  std::vector<float> samples;
  Clock::time_point first;
  bool blocked = false, started = false;
  int writeSamples(const float* data, int count) override
  {
    if (blocked) return 0;
    if (!started) { first = Clock::now(); started = true; }
    int accepted = std::min(count, 73);
    samples.insert(samples.end(), data, data + accepted);
    return accepted;
  }
  void flushSamples() override { sourceAllSamplesFlushed(); }
  void resume() { sourceResumeOutput(); }
};
struct FullSink : Async::AudioSink
{
  std::vector<float> samples;
  int writeSamples(const float* data, int count) override
  { samples.insert(samples.end(), data, data+count); return count; }
  void flushSamples() override { sourceAllSamplesFlushed(); }
};
struct TimedSource : Async::AudioSource
{
  std::vector<float>& input;
  size_t position = 0;
  bool flushed = false;
  explicit TimedSource(std::vector<float>& input) : input(input) {}
  void pump()
  {
    while (position < input.size())
    {
      int n = sinkWriteSamples(input.data() + position, input.size() - position);
      if (n <= 0) return;
      position += n;
    }
    if (!flushed) { flushed = true; sinkFlushSamples(); }
  }
  void resumeOutput() override { pump(); }
  void allSamplesFlushed() override {}
};

int main(int argc, char** argv)
{
  assert(argc == 2);
  int delay = std::stoi(argv[1]);
  Async::CppApplication app;
  PttCtrl controller(delay);
  TimedSink sink;
  controller.registerSink(&sink, false);
  std::vector<float> input(1600);
  for (size_t n=0; n<input.size(); ++n) input[n] = std::sin(n * .037) * .2;
  bool transmitting = false, completed = false;
  Clock::time_point keyed, unkeyed;
  controller.transmitterStateChange.connect([&](bool on) {
    transmitting = on;
    if (on) keyed = Clock::now();
    else { unkeyed = Clock::now(); completed = true; app.quit(); }
  });
  controller.setTxCtrlMode(Tx::TX_AUTO);
  TimedSource source(input);
  source.registerSink(&controller, false);
  // Hold the sink beyond the startup delay and then resume it.
  sink.blocked = true;
  source.pump();
  assert(transmitting && !sink.started);
  Async::Timer resume(delay + 80, Async::Timer::TYPE_ONESHOT);
  resume.expired.connect([&](Async::Timer*) { sink.blocked = false; sink.resume(); });
  Async::Timer deadline(2500, Async::Timer::TYPE_ONESHOT);
  deadline.expired.connect([&](Async::Timer*) { app.quit(); });
  app.exec();
  assert(completed && !transmitting && sink.samples == input);
  double elapsed = std::chrono::duration<double, std::milli>(sink.first-keyed).count();
  assert(elapsed >= delay + 60 && elapsed < delay + 800);
  assert(unkeyed >= sink.first);
  controller.unregisterSink();
  source.unregisterSink();

  // Exercise upstream's chunked S16 decoder with boundary values and a frame
  // larger than its internal stack chunk. Invalid and odd-sized frames too.
  Async::AudioDecoderS16 decoder;
  FullSink decoded;
  decoder.registerSink(&decoded, false);
  std::vector<int16_t> pcm(8193);
  for (size_t n=0; n<pcm.size(); ++n) pcm[n] = static_cast<int16_t>(n * 257);
  pcm[0] = -32768; pcm[1] = 32767; pcm[2] = 0;
  std::vector<unsigned char> encoded(pcm.size()*2 + 1, 0x5a);
  std::memcpy(encoded.data(), pcm.data(), pcm.size()*2);
  decoder.writeEncodedSamples(encoded.data(), encoded.size());
  assert(decoded.samples.size() == pcm.size());
  for (size_t n=0; n<pcm.size(); ++n) assert(decoded.samples[n] == pcm[n]/32768.0f);
  decoder.writeEncodedSamples(pcm.data(), -1);
  decoder.writeEncodedSamples(pcm.data(), 0);
  assert(decoded.samples.size() == pcm.size());
  decoder.unregisterSink();
  std::cout << "PASS PTT: TX_DELAY=" << delay << " ms, first audio=" << elapsed
            << " ms with backpressure, exact samples, PTT released; S16 boundaries/chunks\n";
}
