#include "AiorsLedClient.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
unsigned long long monotonicMs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<unsigned long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

struct CommandFiles {
  std::string input, output, temporary;
  ~CommandFiles() {
    unlink(input.c_str());
    unlink(output.c_str());
    unlink(temporary.c_str());
  }
};
}

std::shared_ptr<AiorsLedClient> AiorsLedClient::shared()
{
  // EventHandlers are constructed/destructed on the application thread.
  static std::weak_ptr<AiorsLedClient> instance;
  auto client = instance.lock();
  if (!client) {
    client = std::make_shared<AiorsLedClient>("/run/aiors/cmd");
    // Clear stale outputs left by a previous SvxLink process.
    for (unsigned led = 0; led < 4; ++led) client->set(led, false);
    instance = client;
  }
  return client;
}

AiorsLedClient::AiorsLedClient(const std::string& directory, Timing timing)
  : directory(directory), timing(timing)
{
  if (!timing.timeout_ms || !timing.refresh_ms || !timing.retry_ms ||
      timing.retry_max_ms < timing.retry_ms)
    throw std::invalid_argument("invalid AIORS LED timings");
  worker = std::thread(&AiorsLedClient::run, this);
}

AiorsLedClient::~AiorsLedClient()
{
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
  }
  changed.notify_one();
  worker.join();
}

void AiorsLedClient::set(unsigned led, bool on)
{
  if (led >= states.size()) return;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto& state = states[led];
    if (state.known && state.desired == on) return;
    state.known = true;
    state.desired = on;
    if (!state.failures) state.due = Clock::now();
  }
  changed.notify_one();
}

bool AiorsLedClient::pause(unsigned milliseconds)
{
  std::unique_lock<std::mutex> lock(mutex);
  return changed.wait_for(lock, std::chrono::milliseconds(milliseconds),
      [this] { return stopping; });
}

bool AiorsLedClient::transact(unsigned led, bool on, std::string& error)
{
  // The BSP protocol uses boot-relative milliseconds. Never generate a future
  // timestamp or reuse a filename when two replies arrive in the same millisecond.
  auto request_ms = monotonicMs();
  while (request_ms <= last_request_ms) {
    if (pause(1)) return false;
    request_ms = monotonicMs();
  }
  last_request_ms = request_ms;
  const std::string input = directory + "/cmd" + std::to_string(getpid()) +
      "_" + std::to_string(request_ms);
  CommandFiles files{input, input + ".out", input + ".led.tmp"};
  const char channel = led < 2 ? 'a' : 'b';
  const char *kind = led % 2 ? "tx" : "rx";
  const char *state = on ? "on" : "off";
  char command[64], expected[64];
  const int length = snprintf(command, sizeof(command), "set led %c %s %s", channel, kind, state);
  snprintf(expected, sizeof(expected), "led %c %s=%s\n", channel, kind, state);

  int fd = open(files.temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) { error = std::string("cannot create command: ") + strerror(errno); return false; }
  bool written = fchmod(fd, 0644) == 0;
  size_t used = 0;
  while (written && used < static_cast<size_t>(length)) {
    ssize_t count = write(fd, command + used, length - used);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { written = false; break; }
    used += count;
  }
  if (close(fd) != 0) written = false;
  // /run is volatile IPC. Atomic rename, with no disk fsync or external process.
  if (!written || rename(files.temporary.c_str(), files.input.c_str()) != 0) {
    error = "cannot publish command";
    return false;
  }

  const auto deadline = Clock::now() + std::chrono::milliseconds(timing.timeout_ms);
  while (Clock::now() < deadline) {
    fd = open(files.output.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
      struct stat st;
      char reply[257];
      bool regular = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 257;
      ssize_t count = regular ? read(fd, reply, sizeof(reply) - 1) : -1;
      close(fd);
      if (count <= 0) { error = "invalid reply file"; return false; }
      reply[count] = '\0';
      if (static_cast<size_t>(count) == strlen(expected) && memcmp(reply, expected, count) == 0) return true;
      error = "unexpected or error reply";
      return false;
    }
    if (errno != ENOENT && errno != EINTR) {
      error = std::string("cannot read reply: ") + strerror(errno);
      return false;
    }
    if (pause(10)) return false;
  }
  error = "reply timeout";
  return false;
}

void AiorsLedClient::run()
{
  unsigned next = 0;
  std::unique_lock<std::mutex> lock(mutex);
  while (!stopping) {
    unsigned selected = states.size();
    auto wake = Clock::time_point::max();
    const auto now = Clock::now();
    for (unsigned n = 0; n < states.size(); ++n) {
      const unsigned i = (next + n) % states.size();
      if (!states[i].known) continue;
      if (states[i].due <= now) { selected = i; break; }
      wake = std::min(wake, states[i].due);
    }
    if (selected == states.size()) { changed.wait_until(lock, wake); continue; }
    next = (selected + 1) % states.size();
    const bool sent = states[selected].desired;
    lock.unlock();
    std::string error;
    const bool success = transact(selected, sent, error);
    lock.lock();
    if (stopping) break;
    auto& state = states[selected];
    const auto completed = Clock::now();
    const bool recovered = success && state.failures != 0;
    const bool warning = !success && (state.failures == 0 ||
        completed - state.last_warning >= std::chrono::seconds(30));
    if (success) {
      state.failures = 0;
      state.due = state.desired != sent ? completed :
          completed + std::chrono::milliseconds(timing.refresh_ms);
    } else {
      unsigned delay = timing.retry_ms;
      for (unsigned n = 0; n < state.failures && delay < timing.retry_max_ms; ++n)
        delay = std::min(timing.retry_max_ms, delay <= timing.retry_max_ms / 2 ? delay * 2 : timing.retry_max_ms);
      if (state.failures < 32) ++state.failures;
      state.due = completed + std::chrono::milliseconds(delay);
      if (warning) state.last_warning = completed;
    }
    lock.unlock();
    if (warning) fprintf(stderr, "AIORS: LED %c %s update failed (%s); retrying in background\n",
        selected < 2 ? 'a' : 'b', selected % 2 ? "tx" : "rx", error.c_str());
    if (recovered) fprintf(stderr, "AIORS: LED %c %s communication recovered\n",
        selected < 2 ? 'a' : 'b', selected % 2 ? "tx" : "rx");
    lock.lock();
  }
}
