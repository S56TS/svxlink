// Test-only, paced sound-card replacement. LocalTx, filters, CTCSS generator,
// PttCtrl and RepeaterLogic remain production implementations.
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <dlfcn.h>
#include <AsyncAudioDevice.h>
#include <AsyncAudioDeviceFactory.h>

static void retainVirtualBackend()
{
  // The sound-card factory outlives logic plugins. SvxLink may unload this
  // fixture logic before a LocalTx using its card is destroyed at shutdown.
  static void* pinned = []() -> void* {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&retainVirtualBackend), &info)) return nullptr;
    return dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
  }();
  if (!pinned) throw std::runtime_error("Cannot retain virtual sound-card backend");
}

class VirtualSoundCard : public Async::AudioDevice
{
public:
  explicit VirtualSoundCard(const std::string& path)
    : AudioDevice(path), pcm(path, std::ios::binary), blocks(path + ".blocks"),
      opens(path + ".opens"),
      timer(10, Async::Timer::TYPE_PERIODIC, false)
  {
    retainVirtualBackend();
    timer.expired.connect([this](Async::Timer*) { pump(); });
  }
  size_t readBlocksize() override { return sampleRate() / 100; }
  size_t writeBlocksize() override { return sampleRate() / 100; }
  bool isFullDuplexCapable() override { return false; }
  void audioToWriteAvailable() override { timer.setEnable(true); }
  void flushSamples() override {}
  int samplesToWrite() const override { return 0; }
protected:
  bool openDevice(Mode mode) override
  {
    opens << micros() << "\n"; opens.flush();
    return mode == MODE_WR && pcm.good();
  }
  void closeDevice() override { timer.setEnable(false); }
private:
  std::ofstream pcm, blocks, opens;
  Async::Timer timer;
  size_t offset = 0;
  static int64_t micros()
  {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void pump()
  {
    std::vector<int16_t> samples(writeBlocksize() * getChannels());
    size_t count = getBlocks(samples.data(), 1) * writeBlocksize();
    if (!count) return;
    blocks << micros() << " " << offset << " " << count << "\n";
    blocks.flush();
    pcm.write(reinterpret_cast<const char*>(samples.data()), count * getChannels() * sizeof(int16_t));
    pcm.flush();
    offset += count;
  }
};
namespace Async { REGISTER_AUDIO_DEVICE_TYPE("virtual", VirtualSoundCard); }

#include "chat_test_logic.cpp"
