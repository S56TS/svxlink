// Async callbacks may destroy the Timer/FdWatch that owns their signal.
#include <cassert>
#include <iostream>
#include <unistd.h>
#include <AsyncCppApplication.h>
#include <AsyncTimer.h>
#include <AsyncFdWatch.h>
#include <AsyncExec.h>

int main()
{
  Async::CppApplication app;
  unsigned completed=0, created=0;
  Async::Timer driver(2,Async::Timer::TYPE_PERIODIC);
  driver.expired.connect([&](Async::Timer*) {
    if (++created==100) driver.setEnable(false);
    auto timer=new Async::Timer(0,Async::Timer::TYPE_ONESHOT);
    timer->expired.connect([&](Async::Timer* expired) {
      delete expired;
      ++completed;
      if (completed==105) app.quit();
    });
  });
  int sockets[2]; assert(pipe(sockets)==0);
  for (auto type : {Async::FdWatch::FD_WATCH_RD,Async::FdWatch::FD_WATCH_WR})
  {
    if (type==Async::FdWatch::FD_WATCH_RD) assert(write(sockets[1],"x",1)==1);
    auto watch=new Async::FdWatch(sockets[type==Async::FdWatch::FD_WATCH_RD?0:1],type);
    watch->activity.connect([&](Async::FdWatch* active) {
      delete active; ++completed;
      if (completed==105) app.quit();
    });
  }
  for (int n=0;n<3;++n)
  {
    auto child=new Async::Exec("/bin/true");
    child->exited.connect([&,child]() {
      assert(child->ifExited() && child->exitStatus()==0);
      delete child;
      if (++completed==105) app.quit();
    });
    assert(child->run());
  }
  app.exec();
  driver.setEnable(false);
  assert(completed==105);
  assert(created==100);
  close(sockets[0]); close(sockets[1]);
  std::cout << "PASS Async callback lifetime: 100 self-deleting timers, read/write watches and Exec exit callbacks\n";
}
