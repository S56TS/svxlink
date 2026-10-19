#ifndef AIORS_LED_CLIENT_INCLUDED
#define AIORS_LED_CLIENT_INCLUDED

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// One worker and four coalesced states for the entire SvxLink process.
// set() only touches memory; all command/reply filesystem I/O stays on the worker.
class AiorsLedClient
{
  public:
    struct Timing {
      unsigned timeout_ms, refresh_ms, retry_ms, retry_max_ms;
    };
    static std::shared_ptr<AiorsLedClient> shared();
    explicit AiorsLedClient(const std::string& directory,
        Timing timing = {2000, 5000, 250, 5000});
    ~AiorsLedClient();
    void set(unsigned led, bool on);
    AiorsLedClient(const AiorsLedClient&) = delete;
    AiorsLedClient& operator=(const AiorsLedClient&) = delete;

  private:
    using Clock = std::chrono::steady_clock;
    struct State {
      bool known = false, desired = false;
      unsigned failures = 0;
      Clock::time_point due{}, last_warning{};
    };
    const std::string directory;
    const Timing timing;
    std::array<State, 4> states;
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::thread worker;
    unsigned long long last_request_ms = 0;
    void run();
    bool transact(unsigned led, bool on, std::string& error);
    bool pause(unsigned milliseconds);
};
#endif
