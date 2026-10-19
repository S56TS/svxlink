#include "AiorsLedClient.h"
extern "C" {
#include "aiors.h"
#include "cli.h"
}
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

static bool all(bool on)
{
  for (unsigned led = 0; led < 4; ++led) {
    bool actual = !on;
    assert(GPIO_GetLed(led < 2 ? 'a' : 'b', led % 2 == 0, &actual) == AIORS_OK);
    if (actual != on) return false;
  }
  return true;
}

static void waitAll(bool on)
{
  for (unsigned i = 0; i < 1000 && !all(on); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(all(on));
}

int main()
{
  AIORS_Cfg_t cfg = {};
  assert(GPIO_Init(&cfg) == AIORS_OK);
  assert(CLI_Init() == AIORS_OK);
  assert(CLI_StartThread(&cfg) == AIORS_OK);
  {
    AiorsLedClient client("/tmp/svxlink-aiors-bridge-test", {250, 150, 20, 80});
    for (unsigned led = 0; led < 4; ++led) client.set(led, true);
    waitAll(true);
    CLI_Close();
    for (unsigned led = 0; led < 4; ++led) client.set(led, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    assert(CLI_StartThread(&cfg) == AIORS_OK);
    waitAll(false);
    // An aiorsd reset must be repaired without another hook event.
    for (char ch : {'a', 'b'}) {
      assert(GPIO_SetLed(ch, true, 1, nullptr) == AIORS_OK);
      assert(GPIO_SetLed(ch, false, 1, nullptr) == AIORS_OK);
    }
    waitAll(false);
  }
  CLI_Close();
  GPIO_Close();
  rmdir("/tmp/svxlink-aiors-bridge-test");
  puts("Native SvxLink worker -> real BSP CLI -> GPIO: all LEDs, daemon outage/restart and resync passed");
}
