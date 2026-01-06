/**
@file	 SquelchHidraw.cpp
@brief   A squelch detector that read squelch state from a linux/hidraw
         device
@author  Adi Bier / DL1HRC
@date	 2014-09-17

\verbatim
SvxLink - A Multi Purpose Voice Services System for Ham Radio Use
Copyright (C) 2003-2014 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncFdWatch.h>
#include <AsyncTimer.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "SquelchHidraw.h"



/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;



/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

SquelchHidraw::SquelchHidraw(void)
  : fd(-1), watch(0), reopen_timer(0), device(), rx_name(),
    reopen_delay_ms(250), disconnected_logged(false),
    active_low(false), pin(0)
{
} /* SquelchHidraw::SquelchHidraw */


SquelchHidraw::~SquelchHidraw(void)
{
  delete reopen_timer;
  reopen_timer = 0;
  closeDevice();
} /* SquelchHidraw::~SquelchHidraw */


/**
Initializing the sound card as linux/hidraw device
For further information:
  http://dmkeng.com
  http://www.halicky.sk/om3cph/sb/CM108_DataSheet_v1.6.pdf
  http://www.ti.com/lit/ml/sllu093/sllu093.pdf
  http://www.ti.com/tool/usb-to-gpio
*/
bool SquelchHidraw::initialize(Async::Config& cfg, const std::string& rx_name)
{
  if (!Squelch::initialize(cfg, rx_name))
  {
    return false;
  }

  this->rx_name = rx_name;

  if (!cfg.getValue(rx_name, "HID_DEVICE", device) || device.empty())
  {
    cerr << "*** ERROR: Config variable " << rx_name
         << "/HID_DEVICE not set or invalid" << endl;
    return false;
  }

  string sql_pin;
  if (!cfg.getValue(rx_name, "HID_SQL_PIN", sql_pin) || sql_pin.empty())
  {
    cerr << "*** ERROR: Config variable " << rx_name
         << "/HID_SQL_PIN not set or invalid\n";
    return false;
  }

  if ((sql_pin.size() > 1) && (sql_pin[0] == '!'))
  {
    active_low = true;
    sql_pin.erase(0, 1);
  }

  map<string, char> pin_mask;
  pin_mask["VOL_UP"] = 0x01;
  pin_mask["VOL_DN"] = 0x02;
  pin_mask["MUTE_PLAY"] = 0x04;
  pin_mask["MUTE_REC"] = 0x08;

  map<string, char>::iterator it = pin_mask.find(sql_pin);
  if (it == pin_mask.end())
  {
    cerr << "*** ERROR: Invalid value for " << rx_name << "/HID_SQL_PIN="
         << sql_pin << ", must be VOL_UP, VOL_DN, MUTE_PLAY, MUTE_REC" << endl;
    return false;
  }
  pin = (*it).second;

  if (!openDevice())
  {
    // Do not fail hard. USB devices may appear a bit later or after re-enum.
    // We'll retry in the background.
    scheduleReopen(reopen_delay_ms);
    return true;
  }

  struct hidraw_devinfo hiddevinfo;
  if ((ioctl(fd, HIDIOCGRAWINFO, &hiddevinfo) != -1) &&
      (hiddevinfo.vendor == 0x0d8c))
  {
    cout << "--- Hidraw sound chip is ";
    if (hiddevinfo.product == 0x000c)
    {
      cout << "CM108";
    }
    else if (hiddevinfo.product == 0x013c)
    {
      cout << "CM108A";
    }
    else if (hiddevinfo.product == 0x0012)
    {
      cout << "CM108B";
    }
    else if (hiddevinfo.product == 0x000e)
    {
      cout << "CM109";
    }
    else if (hiddevinfo.product == 0x013a)
    {
      cout << "CM119";
    }
    else if (hiddevinfo.product == 0x0013)
    {
      cout << "CM119A";
    }
    else
    {
      cout << "unknown";
    }
    cout << endl;
  }
  else
  {
    cout << "*** ERROR: unknown/unsupported sound chip detected...\n";
    return false;
  }

  // Setup retry timer used for USB disconnect/reconnect recovery
  reopen_timer = new Async::Timer(1000, Async::Timer::TYPE_ONESHOT);
  reopen_timer->setEnable(false);
  reopen_timer->expired.connect(mem_fun(*this, &SquelchHidraw::tryReopen));

  return true;
}


/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

/**
 * @brief  Called when state of Hidraw port has been changed
 */
void SquelchHidraw::hidrawActivity(FdWatch *watch)
{
  (void)watch;

  char buf[5];
  int rd = read(fd, buf, sizeof(buf));
  if (rd <= 0)
  {
    // Typical after USB disconnect: read() returns -1 with ENODEV/EIO, or 0.
    if (!disconnected_logged)
    {
      if (rd == 0)
      {
        cerr << "*** ERROR: reading HID_DEVICE (" << device
             << ") returned EOF -- will retry" << endl;
      }
      else
      {
        cerr << "*** ERROR: reading HID_DEVICE (" << device << ") failed: "
             << strerror(errno) << " -- will retry" << endl;
      }
      disconnected_logged = true;
    }

    // Consider squelch closed while device is missing
    setSignalDetected(false);

    closeDevice();
    scheduleReopen(reopen_delay_ms);
    return;
  }

  bool pin_high = buf[0] & pin;
  setSignalDetected(pin_high != active_low);
} /* SquelchHidraw::hidrawActivity */


void SquelchHidraw::closeDevice(void)
{
  if (watch != 0)
  {
    delete watch;
    watch = 0;
  }
  if (fd >= 0)
  {
    close(fd);
    fd = -1;
  }
}


bool SquelchHidraw::openDevice(void)
{
  closeDevice();

  fd = open(device.c_str(), O_RDWR, 0);
  if (fd < 0)
  {
    return false;
  }

  // Verify chip (keep the existing behavior)
  struct hidraw_devinfo hiddevinfo;
  if ((ioctl(fd, HIDIOCGRAWINFO, &hiddevinfo) == -1) ||
      (hiddevinfo.vendor != 0x0d8c))
  {
    closeDevice();
    return false;
  }

  watch = new Async::FdWatch(fd, Async::FdWatch::FD_WATCH_RD);
  assert(watch != 0);
  watch->activity.connect(mem_fun(*this, &SquelchHidraw::hidrawActivity));

  disconnected_logged = false;
  reopen_delay_ms = 250;

  return true;
}


void SquelchHidraw::scheduleReopen(unsigned delay_ms)
{
  if (reopen_timer == 0)
  {
    reopen_timer = new Async::Timer(1000, Async::Timer::TYPE_ONESHOT);
    reopen_timer->setEnable(false);
    reopen_timer->expired.connect(mem_fun(*this, &SquelchHidraw::tryReopen));
  }

  // Cap delay to avoid super long pauses
  if (delay_ms < 100) delay_ms = 100;
  if (delay_ms > 5000) delay_ms = 5000;

  reopen_timer->setTimeout(delay_ms);
  reopen_timer->setEnable(true);
}


void SquelchHidraw::tryReopen(Async::Timer *t)
{
  (void)t;

  if (openDevice())
  {
    cout << "--- HID_DEVICE reconnected: " << device << endl;
    return;
  }

  // Backoff
  if (reopen_delay_ms < 5000)
  {
    reopen_delay_ms = (reopen_delay_ms < 1000)
                        ? (reopen_delay_ms * 2)
                        : (reopen_delay_ms + 1000);
  }
  scheduleReopen(reopen_delay_ms);
}



/*
 * This file has not been truncated
 */
