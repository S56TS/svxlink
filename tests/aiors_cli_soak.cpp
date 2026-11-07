// Sustained actual BSP CLI + SvxLink worker test. Only GPIO uses a stub.
#include "AiorsLedClient.h"
extern "C" {
#include "aiors.h"
#include "cli.h"
}
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <malloc.h>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

extern "C" size_t __sanitizer_get_current_allocated_bytes() __attribute__((weak));

static uint64_t last_request;
static unsigned requests, cycles, restarts, abandoned, expired, slow_jobs, resyncs;
static uint64_t max_reply_ms, max_recovery_ms, max_burst_us;
static uint64_t live_baseline, live_last, live_peak;
static void waitFor(const std::function<bool()>& condition, unsigned ms=3000)
{
  const auto deadline=UTIL_Millis()+ms;
  while (!condition() && UTIL_Millis()<deadline) UTIL_SleepMs(5);
  assert(condition());
}
static std::string input()
{
  uint64_t now=UTIL_Millis();
  while (now<=last_request) { UTIL_SleepMs(1); now=UTIL_Millis(); }
  last_request=now;
  return std::string(CMD_DIR)+"/cmd"+std::to_string(getpid())+"_"+std::to_string(now);
}
static std::string submit(const std::string& command)
{
  const auto path=input();
  assert(UTIL_WriteTextFileAtomic(path.c_str(),command.data(),command.size())==AIORS_OK);
  ++requests;
  return path+".out";
}
static std::string reply(const std::string& path, unsigned timeout=1500)
{
  const auto start=UTIL_Millis();
  waitFor([&] { return access(path.c_str(),F_OK)==0; },timeout);
  max_reply_ms=std::max(max_reply_ms,UTIL_Millis()-start);
  char text[4096];
  assert(UTIL_ReadTextFile(path.c_str(),text,sizeof(text))==AIORS_OK);
  unlink(path.c_str());
  return text;
}
static bool leds(unsigned mask)
{
  for (unsigned led=0; led<4; ++led) {
    bool state=false;
    assert(GPIO_GetLed(led<2?'a':'b',led%2==0,&state)==AIORS_OK);
    if (state!=bool(mask&(1U<<led))) return false;
  }
  return true;
}
static void set(AiorsLedClient& client, unsigned mask)
{
  for (unsigned led=0; led<4; ++led) client.set(led,bool(mask&(1U<<led)));
}
static unsigned files()
{
  unsigned count=0;
  DIR *dir=opendir(CMD_DIR); assert(dir);
  while (auto entry=readdir(dir)) if (entry->d_name[0]!='.') ++count;
  closedir(dir); return count;
}
int main(int argc, char **argv)
{
  assert(argc==2 || argc==3);
  const unsigned seconds=std::stoul(argv[1]); assert(seconds>0);
  const unsigned outage_ms=argc==3 ? std::stoul(argv[2]) : 2300;
  assert(outage_ms>2000);
  setvbuf(stdout,nullptr,_IOLBF,0);
  AIORS_Cfg_t cfg={};
  UTIL_UptimeStart();
  assert(GPIO_Init(&cfg)==AIORS_OK);
  assert(SA818_Init(&cfg)==AIORS_OK);
  assert(CLI_Init()==AIORS_OK);
  assert(CLI_StartThread(&cfg)==AIORS_OK);
  const auto start=UTIL_Millis();
  auto next_fault=start+10000;
  std::vector<std::string> leftovers;
  {
    // Keep production deadlines, refresh and backoff. Accelerated event bursts
    // stress coalescing, while reply expiry still uses the real ten seconds.
    AiorsLedClient client(CMD_DIR);
    set(client,0); waitFor([&] { return leds(0); });
    while (UTIL_Millis()-start<uint64_t(seconds)*1000) {
      const unsigned mask=(cycles+1)%16;
      set(client,mask); waitFor([&] { return leds(mask); });
      const auto version=submit("version");
      const auto diagnostics=submit("get diagnostics all");
      const auto state=submit("get led all");
      const auto invalid=submit("not a command");
      assert(reply(version).find("aiorsd ")==0);
      const auto diag=reply(diagnostics);
      assert(diag.find("diagnostics_version=5\n")==0);
      assert(diag.find("\noverall_status=degraded\n")!=std::string::npos);
      std::string expected;
      for (unsigned led=0; led<4; ++led) {
        expected+="led "; expected+=led<2?'a':'b';
        expected+=led%2?" tx=":" rx=";
        expected+=(mask&(1U<<led))?"on\n":"off\n";
      }
      assert(reply(state)==expected);
      assert(reply(invalid)=="err: unknown command\n");
      // Leave an actual reply unread with THIS still-running process as owner.
      const auto stale=submit("version");
      waitFor([&] { return access(stale.c_str(),F_OK)==0; });
      leftovers.push_back(stale); ++abandoned;
      // Also leave a temporary artifact; both must expire without client exit.
      const auto artifact=input()+".led.tmp";
      assert(UTIL_WriteTextFileAtomic(artifact.c_str(),"unfinished",10)==AIORS_OK);
      leftovers.push_back(artifact); ++abandoned;
      for (auto it=leftovers.begin(); it!=leftovers.end();) {
        if (access(it->c_str(),F_OK)!=0) { it=leftovers.erase(it); ++expired; }
        else ++it;
      }
      assert(files()<160); // ten seconds of deliberate leftovers plus active IPC
      assert(UTIL_Millis()-CLI_LastProgressMs()<1500);
      if (UTIL_Millis()>=next_fault) {
        // Real USB command dispatch/capture, with harmless one-second helper.
        const auto slow=submit("get usb power 2"); ++slow_jobs;
        UTIL_SleepMs(100);
        const auto fast_start=UTIL_Millis();
        assert(reply(submit("version")).find("aiorsd ")==0);
        assert(UTIL_Millis()-fast_start<700);
        const auto slow_reply=reply(slow,2500);
        assert(slow_reply.find("power=on")!=std::string::npos ||
               slow_reply.find("power 2=on")!=std::string::npos);
        // Keep the same worker and daemon process alive across worker restarts.
        CLI_Close(); ++restarts;
        const auto burst=std::chrono::steady_clock::now();
        for (unsigned i=0;i<50000;++i) client.set(i%4,(i/4)%2);
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-burst).count();
        max_burst_us=std::max(max_burst_us,uint64_t(us)); assert(us<1000000);
        set(client,15);
        UTIL_SleepMs(outage_ms); // force the production two-second reply timeout
        const auto recovery=UTIL_Millis();
        assert(CLI_StartThread(&cfg)==AIORS_OK);
        waitFor([&] { return leds(15); },8000);
        max_recovery_ms=std::max(max_recovery_ms,UTIL_Millis()-recovery);
        // Reset outputs without a new set event: production five-second refresh.
        for (char channel : {'a','b'}) {
          assert(GPIO_SetLed(channel,true,0,nullptr)==AIORS_OK);
          assert(GPIO_SetLed(channel,false,0,nullptr)==AIORS_OK);
        }
        waitFor([&] { return leds(15); },6500); ++resyncs;
        next_fault=UTIL_Millis()+10000;
      }
      ++cycles;
      live_last=__sanitizer_get_current_allocated_bytes ?
          __sanitizer_get_current_allocated_bytes() : mallinfo2().uordblks;
      if (UTIL_Millis()-start>=120000) {
        if (!live_baseline) live_baseline=live_last;
        live_peak=std::max(live_peak,live_last);
      }
      if (cycles%25==0) printf("CLI progress: %u cycles, %u requests, %u restarts, %u expired\n",
          cycles,requests,restarts,expired);
      UTIL_SleepMs(100);
    }
    // Keep the daemon scanning after traffic stops. Every live-owner orphan
    // must vanish naturally; the harness does not unlink these leftovers.
    for (const auto& path:leftovers) {
      waitFor([&] { return access(path.c_str(),F_OK)!=0; },12000); ++expired;
    }
    assert(expired==abandoned);
    assert(!live_baseline || live_last<=live_baseline+65536);
  }
  // Actual SvxLink diagnostics may still issue requests in this directory.
  // Parent stops SvxLink before cleaning its private temporary root.
  CLI_Close(); SA818_Close(); GPIO_Close();
  printf("CLI_RESULT {\"cycles\":%u,\"requests\":%u,\"restarts\":%u,"
         "\"abandoned\":%u,\"expired\":%u,\"slow_jobs\":%u,\"resyncs\":%u,"
         "\"max_reply_ms\":%llu,\"max_recovery_ms\":%llu,\"max_burst_us\":%llu,"
         "\"live_baseline_bytes\":%llu,\"live_last_bytes\":%llu,\"live_peak_bytes\":%llu,"
         "\"allocation_measurement\":\"%s\","
         "\"outage_ms\":%u,"
         "\"elapsed_ms\":%llu}\n",cycles,requests,restarts,abandoned,expired,slow_jobs,resyncs,
         (unsigned long long)max_reply_ms,(unsigned long long)max_recovery_ms,
         (unsigned long long)max_burst_us,(unsigned long long)live_baseline,
         (unsigned long long)live_last,(unsigned long long)live_peak,
         __sanitizer_get_current_allocated_bytes ? "asan" : "guest-mallinfo2",
         outage_ms,
         (unsigned long long)(UTIL_Millis()-start));
}
