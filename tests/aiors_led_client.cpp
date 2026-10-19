#include "AiorsLedClient.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static void eventually(const std::function<bool()>& condition, int milliseconds = 3000)
{
  auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
  while (!condition() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(condition());
}

static unsigned countFiles(const std::string& directory)
{
  DIR *dir = opendir(directory.c_str());
  assert(dir);
  unsigned count = 0;
  while (auto *entry = readdir(dir)) if (entry->d_name[0] != '.') ++count;
  closedir(dir);
  return count;
}

struct Daemon {
  std::string directory;
  std::array<std::atomic<bool>, 4> leds;
  std::atomic<bool> running{true};
  std::atomic<int> mode{0}, attempts{0}, commands{0};
  // 0=normal, 1=GPIO error, 2=wrong ack, 3=no reply, 4=oversized,
  // 5=FIFO reply, 6=slow valid reply.
  std::thread thread;
  explicit Daemon(const std::string& directory) : directory(directory) {
    for (auto& led : leds) led = false;
    thread = std::thread([this] { run(); });
  }
  ~Daemon() { running = false; thread.join(); }
  void run() {
    while (running) {
      DIR *dir = opendir(directory.c_str());
      if (dir) {
        while (auto *entry = readdir(dir)) {
          std::string name(entry->d_name);
          if (name.compare(0, 3, "cmd") || name.find('.') != std::string::npos) continue;
          std::string path = directory + "/" + name;
          std::ifstream file(path);
          std::string line;
          std::getline(file, line);
          if (line.empty()) continue;
          unlink(path.c_str());
          char ch[8], kind[8], state[8];
          assert(sscanf(line.c_str(), "set led %7s %7s %7s", ch, kind, state) == 3);
          int selected = (strcmp(ch, "b") == 0 ? 2 : 0) + (strcmp(kind, "tx") == 0 ? 1 : 0);
          bool value = strcmp(state, "on") == 0;
          ++attempts;
          const int behavior = mode.load();
          if (behavior == 3) continue;
          if (behavior == 5) { assert(mkfifo((path + ".out").c_str(), 0600) == 0); continue; }
          std::string result;
          if (behavior == 1) result = "err: gpio\n";
          else if (behavior == 2) result = std::string("led ") + ch + " " + kind + "=" + (value ? "off\n" : "on\n");
          else if (behavior == 4) result.assign(4096, 'x');
          else {
            if (behavior == 6) std::this_thread::sleep_for(std::chrono::milliseconds(60));
            leds[selected] = value;
            ++commands;
            result = std::string("led ") + ch + " " + kind + "=" + state + "\n";
          }
          std::ofstream reply(path + ".reply");
          reply << result;
          reply.close();
          assert(rename((path + ".reply").c_str(), (path + ".out").c_str()) == 0);
        }
        closedir(dir);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
};

int main()
{
  char base[] = "/tmp/svxlink-led-test-XXXXXX";
  assert(mkdtemp(base));
  const std::string directory = std::string(base) + "/cmd";
  const unsigned fd_before = countFiles("/proc/self/fd");
  {
    AiorsLedClient client(directory, {150, 200, 15, 60});
    // Missing directory/daemon must not turn 20,000 event callbacks into I/O waits.
    const auto begin = Clock::now();
    for (unsigned i = 0; i < 20000; ++i) client.set(i % 4, (i / 4) % 2);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
    assert(elapsed < 1000);
    printf("20,000 queued LED events with IPC unavailable: %ld ms\n", static_cast<long>(elapsed));
    for (unsigned led = 0; led < 4; ++led) client.set(led, true);
    assert(mkdir(directory.c_str(), 0700) == 0);
    Daemon daemon(directory);
    eventually([&] { for (auto& led : daemon.leds) if (!led) return false; return true; });

    // Restart/reset aiorsd: periodic refresh must restore all outputs, with no new events.
    for (auto& led : daemon.leds) led = false;
    eventually([&] { for (auto& led : daemon.leds) if (!led) return false; return true; });

    for (int mode = 1; mode <= 5; ++mode) {
      daemon.mode = mode;
      int before = daemon.attempts;
      client.set(0, false);
      eventually([&] { return daemon.attempts >= before + 2; });
      daemon.mode = 0;
      eventually([&] { return !daemon.leds[0]; });
      client.set(0, true);
      eventually([&] { return daemon.leds[0].load(); });
    }

    // A slow command must never hold the queue lock or block event callbacks.
    daemon.mode = 6;
    client.set(0, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto burst = Clock::now();
    for (unsigned i = 0; i < 50000; ++i) client.set(i % 4, (i / 4) % 2);
    assert(Clock::now() - burst < std::chrono::seconds(1));
    for (unsigned led = 0; led < 4; ++led) client.set(led, false);
    eventually([&] { for (auto& led : daemon.leds) if (led) return false; return true; });
    daemon.mode = 0;

    // Sustained updates: latest state wins; descriptors/files remain bounded.
    for (unsigned round = 0; round < 150; ++round) {
      for (unsigned led = 0; led < 4; ++led) client.set(led, round % 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    for (unsigned led = 0; led < 4; ++led) client.set(led, true);
    eventually([&] { for (auto& led : daemon.leds) if (!led) return false; return true; });
    assert(countFiles(directory) <= 2);
    assert(countFiles("/proc/self/fd") <= fd_before + 4);
  }
  assert(countFiles("/proc/self/fd") == fd_before);
  assert(countFiles(directory) == 0);
  // Shutdown interrupts a missing-daemon reply wait rather than waiting for its deadline.
  auto client = std::unique_ptr<AiorsLedClient>(new AiorsLedClient(directory, {2000, 200, 15, 60}));
  client->set(0, true);
  eventually([&] { return countFiles(directory) != 0; });
  const auto stop = Clock::now();
  client.reset();
  assert(Clock::now() - stop < std::chrono::milliseconds(250));
  assert(countFiles(directory) == 0);
  assert(rmdir(directory.c_str()) == 0);
  assert(rmdir(base) == 0);
  puts("AIORS LED client: restart, retry, error replies, FIFO, timeout, coalescing, cleanup and shutdown passed");
}
