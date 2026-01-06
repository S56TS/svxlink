/**
@file	 PttHidraw.h
@brief   A PTT hardware controller using the hidraw device
@author  Tobias Blomberg / SM0SVX & Adi Bier / DL1HRC
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

#ifndef PTT_HIDRAW_INCLUDED
#define PTT_HIDRAW_INCLUDED


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <string>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "Ptt.h"


/****************************************************************************
 *
 * Class definitions
 *
 ****************************************************************************/

class PttHidraw : public Ptt
{
  public:
    struct Factory : public PttFactory<PttHidraw>
    {
      Factory(void) : PttFactory<PttHidraw>("Hidraw") {}
    };

    PttHidraw(void);
    ~PttHidraw(void);

    virtual bool initialize(Async::Config &cfg, const std::string name);
    virtual bool setTxOn(bool tx_on);

  protected:

  private:
    bool active_low;

    std::string device;
    bool        disconnected_logged;

    int   fd;
    char  pin;

    bool openDevice(void);
    void closeDevice(void);

    PttHidraw(const PttHidraw&);
    PttHidraw& operator=(const PttHidraw&);

};  /* class PttHidraw */


#endif /* PTT_HIDRAW_INCLUDED */


/*
 * This file has not been truncated
 */
