// Production HIDRAW drivers with syscall fault injection; no USB hardware.
#include <bits/stdc++.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <AsyncCppApplication.h>
#include <AsyncConfig.h>
#include <AsyncTimer.h>
#include <AsyncFdWatch.h>
#define private public
#define protected public
#include "PttHidraw.h"
#include "SquelchHidraw.h"
#undef private
#undef protected

static bool missing = false, fail_write = false, eof_read = false;
static bool fail_read = false, interrupt_read = false;
static bool short_write = false, interrupt_write = false;
static unsigned char input = 0;
static std::vector<unsigned char> report;
static std::set<int> devices;

extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_close(int);
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int __wrap_open(const char* path, int flags, ...)
{
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  if (std::string(path) != "/test/hidraw") return __real_open(path, flags, mode);
  if (missing) { errno = ENOENT; return -1; }
  int fd = __real_open("/dev/null", flags);
  if (fd >= 0) devices.insert(fd);
  return fd;
}
extern "C" int __wrap_close(int fd)
{
  devices.erase(fd);
  return __real_close(fd);
}
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
  va_list ap; va_start(ap, request); void* data = va_arg(ap, void*); va_end(ap);
  if (!devices.count(fd)) return __real_ioctl(fd, request, data);
  assert(request == HIDIOCGRAWINFO);
  auto* info = static_cast<hidraw_devinfo*>(data);
  *info = {}; info->vendor = 0x0d8c; info->product = 0x0013;
  return 0;
}
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size)
{
  if (!devices.count(fd)) return __real_write(fd, data, size);
  if (interrupt_write) { interrupt_write = false; errno = EINTR; return -1; }
  if (fail_write) { errno = ENODEV; return -1; }
  if (short_write) return size - 1;
  const auto* bytes = static_cast<const unsigned char*>(data);
  report.assign(bytes, bytes + size);
  return size;
}
extern "C" ssize_t __wrap_read(int fd, void* data, size_t size)
{
  if (!devices.count(fd)) return __real_read(fd, data, size);
  if (interrupt_read) { errno = EINTR; return -1; }
  if (fail_read) { errno = ENODEV; return -1; }
  if (eof_read) return 0;
  assert(size >= 1); static_cast<unsigned char*>(data)[0] = input;
  return 1;
}

int main()
{
  Async::CppApplication app;
  Async::Config cfg;
  cfg.setValue("Tx", "HID_DEVICE", "/test/hidraw");
  cfg.setValue("Rx", "HID_DEVICE", "/test/hidraw");
  cfg.setValue("Rx", "HID_SQL_PIN", "!VOL_UP");
  for (const auto& pin : {std::string("GPIO3"), std::string("!GPIO3")})
  {
    cfg.setValue("Tx", "HID_PTT_PIN", pin);
    PttHidraw ptt;
    assert(ptt.initialize(cfg, "Tx"));
    bool inverted = pin[0] == '!';
    for (bool on : {false, true})
    {
      assert(ptt.setTxOn(on));
      assert(report.size() == 5 && report[3] == 4);
      assert(report[2] == ((on != inverted) ? 4 : 0));
    }
    fail_write = true;
    assert(!ptt.setTxOn(true));
    assert(ptt.m_fd == -1);
    fail_write = false; missing = true;
    assert(!ptt.setTxOn(true));
    missing = false;
    assert(ptt.setTxOn(true));
    assert(report[3] == 4 && report[2] == (inverted ? 0 : 4));
    interrupt_write = true;
    assert(ptt.setTxOn(false));
    short_write = true;
    assert(!ptt.setTxOn(true) && ptt.m_fd == -1);
    short_write = false;
    assert(ptt.setTxOn(false));
    assert(report[3] == 4 && report[2] == (inverted ? 4 : 0));
  }
  cfg.setValue("Tx", "HID_PTT_PIN", "!");
  { PttHidraw invalid; assert(!invalid.initialize(cfg, "Tx")); }
  {
    SquelchHidraw sql;
    missing = true;
    assert(sql.initialize(cfg, "Rx"));
    assert(sql.m_fd == -1 && sql.m_reopen_timer.isEnabled());
    assert(sql.m_reopen_delay_ms == 500);
    for (int n = 0; n < 12; ++n) sql.tryReopen();
    assert(sql.m_reopen_delay_ms == 5000);
    missing = false; sql.tryReopen();
    assert(sql.m_fd >= 0 && !sql.m_reopen_timer.isEnabled());
    assert(sql.m_reopen_delay_ms == 250 && sql.m_active_low);
    input = 0; sql.hidrawActivity(); assert(sql.signalDetected());
    interrupt_read = true; sql.hidrawActivity();
    assert(sql.signalDetected() && sql.m_fd >= 0);
    interrupt_read = false;
    eof_read = true; sql.hidrawActivity();
    assert(!sql.signalDetected() && sql.m_fd == -1 && sql.m_reopen_timer.isEnabled());
    eof_read = false; sql.tryReopen();
    input = 0; sql.hidrawActivity(); assert(sql.signalDetected());
    fail_read = true; sql.hidrawActivity();
    assert(!sql.signalDetected() && sql.m_fd == -1);
    fail_read = false; sql.tryReopen();
    input = 1; sql.hidrawActivity(); assert(!sql.signalDetected());
  }
  cfg.setValue("Rx", "HID_SQL_PIN", "bad");
  { SquelchHidraw invalid; assert(!invalid.initialize(cfg, "Rx")); }
  assert(devices.empty());
  std::cout << "PASS HIDRAW: polarity, first reconnect report, short/EINTR writes, "
               "startup retry, capped backoff, EOF/error recovery, squelch fail-closed\n";
}
