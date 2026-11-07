// Fault boundaries for the actual BSP CLI, with no physical devices initialized.
#include "AiorsLedClient.h"
extern "C" {
#include "aiors.h"
#include "cli.h"
int __real_clock_gettime(clockid_t, timespec*);
}
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static std::atomic<uint64_t> extra_seconds{0};
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec *ts)
{
  int result=__real_clock_gettime(clock,ts);
  if (result==0 && clock==CLOCK_MONOTONIC) ts->tv_sec+=extra_seconds.load();
  return result;
}
static uint64_t last_request;
static std::string submit(const char *command)
{
  uint64_t now=UTIL_Millis();
  while (now<=last_request) { UTIL_SleepMs(1); now=UTIL_Millis(); }
  last_request=now;
  const auto path=std::string(CMD_DIR)+"/cmd"+std::to_string(getpid())+"_"+std::to_string(now);
  assert(UTIL_WriteTextFileAtomic(path.c_str(),command,strlen(command))==AIORS_OK);
  return path;
}
static std::string reply(const std::string& input, unsigned timeout=1500)
{
  const auto output=input+".out";
  const auto deadline=UTIL_Millis()+timeout;
  while (access(output.c_str(),F_OK)!=0 && UTIL_Millis()<deadline) UTIL_SleepMs(5);
  char text[4096];
  assert(UTIL_ReadTextFile(output.c_str(),text,sizeof(text))==AIORS_OK);
  unlink(output.c_str());
  return text;
}
int main()
{
  AIORS_Cfg_t cfg={};
  assert(GPIO_Init(&cfg)==AIORS_OK);
  assert(SA818_Init(&cfg)==AIORS_OK);
  assert(CLI_Init()==AIORS_OK);
  assert(CLI_StartThread(&cfg)==AIORS_OK);
  {
    AiorsLedClient client(CMD_DIR,{500,100,20,80});
    // Actual monotonic filename values cross signed/unsigned 32-bit ranges.
    // Both producer and parser read the same shifted CLOCK_MONOTONIC; real
    // timeout waits remain real time, and no system clock is changed.
    for (uint64_t days : {0ULL,25ULL,50ULL,500ULL}) {
      extra_seconds=days*86400;
      client.set(0,true);
      const auto end=UTIL_Millis()+3000;
      bool led=false;
      do {
        assert(GPIO_GetLed('a',true,&led)==AIORS_OK);
        if (!led) UTIL_SleepMs(5);
      } while (!led && UTIL_Millis()<end);
      assert(led);
      assert(reply(submit("version")).find("aiorsd ")==0);
      assert(GPIO_SetLed('a',true,0,nullptr)==AIORS_OK);
      // No new event: the same worker must keep refreshing at huge uptime.
      // A clock jump can expire a request already in flight; allow its bounded
      // timeout/retry before requiring the periodic refresh to repair output.
      const auto refresh_end=UTIL_Millis()+3000;
      do {
        UTIL_SleepMs(5);
        assert(GPIO_GetLed('a',true,&led)==AIORS_OK);
      } while (!led && UTIL_Millis()<refresh_end);
      assert(GPIO_GetLed('a',true,&led)==AIORS_OK && led);
    }
    // A live RSSI stream survives reply expiration, then expires on stream stop.
    CLI_Close();
    const auto old=UTIL_Millis()-15000;
    const auto base=std::string(CMD_DIR)+"/cmd"+std::to_string(getpid())+"_";
    const auto stream=base+std::to_string(old)+".out";
    const auto orphan=base+std::to_string(old-1)+".out";
    const auto expired_input=base+std::to_string(old-2);
    const auto future_input=base+std::to_string(UTIL_Millis()+60000);
    assert(UTIL_WriteTextFileAtomic(stream.c_str(),"rssi a=on\n",10)==AIORS_OK);
    assert(SA818_SetRssiReportDest(stream.c_str())==AIORS_OK);
    assert(SA818_SetRssiReport(AIORS_SYSTEM_CH_A,true)==AIORS_OK);
    assert(UTIL_WriteTextFileAtomic(orphan.c_str(),"late\n",5)==AIORS_OK);
    assert(UTIL_WriteTextFileAtomic(expired_input.c_str(),"version",7)==AIORS_OK);
    assert(UTIL_WriteTextFileAtomic(future_input.c_str(),"version",7)==AIORS_OK);
    assert(CLI_StartThread(&cfg)==AIORS_OK);
    UTIL_SleepMs(250);
    assert(access(stream.c_str(),F_OK)==0);
    assert(access(orphan.c_str(),F_OK)!=0);
    assert(access(expired_input.c_str(),F_OK)!=0 && access((expired_input+".out").c_str(),F_OK)!=0);
    assert(access(future_input.c_str(),F_OK)!=0 && access((future_input+".out").c_str(),F_OK)!=0);
    SA818_StopRssiReportForDest(stream.c_str());
    UTIL_SleepMs(250);
    assert(access(stream.c_str(),F_OK)!=0);
    // Saturate the bounded slow queue. Fast requests still complete; the worker
    // rejects overflow with "busy" and drops jobs that exceed their deadline.
    std::vector<std::string> jobs;
    for (int i=0;i<12;++i) jobs.push_back(submit("get usb power 2"));
    UTIL_SleepMs(100);
    const auto fast=UTIL_Millis();
    assert(reply(submit("get diagnostics all")).find("diagnostics_version=5\n")==0);
    assert(UTIL_Millis()-fast<700);
    unsigned busy=0;
    UTIL_SleepMs(3500);
    for (const auto& job:jobs) {
      if (access((job+".out").c_str(),F_OK)==0) {
        const auto text=reply(job);
        if (text=="err: busy\n") ++busy;
        else assert(text=="usb 2 power=on\n");
      }
      assert(access(job.c_str(),F_OK)!=0);
    }
    assert(busy>=6);
    assert(reply(submit("version")).find("aiorsd ")==0);
    // Loss/recreation of volatile IPC must not require restarting the LED
    // worker. The daemon advertises directory failure while scanning retries.
    CLI_Close();
    const auto offline=std::string(CMD_DIR)+"-offline";
    assert(rename(CMD_DIR,offline.c_str())==0);
    assert(CLI_StartThread(&cfg)==AIORS_OK);
    UTIL_SleepMs(300);
    assert(!CLI_DirectoryOk());
    assert(GPIO_SetLed('a',true,0,nullptr)==AIORS_OK);
    assert(CLI_Init()==AIORS_OK);
    const auto recover_end=UTIL_Millis()+3000;
    bool repaired=false;
    do {
      assert(GPIO_GetLed('a',true,&repaired)==AIORS_OK);
      if (!repaired) UTIL_SleepMs(5);
    } while (!repaired && UTIL_Millis()<recover_end);
    assert(repaired && CLI_DirectoryOk());
    assert(reply(submit("version")).find("aiorsd ")==0);
  }
  CLI_Close(); SA818_Close(); GPIO_Close();
  puts("PASS CLI boundaries: 25/50/500-day timestamps, active stream preservation, stale/future requests, saturated slow queue, fast-path and IPC-directory recovery");
}
