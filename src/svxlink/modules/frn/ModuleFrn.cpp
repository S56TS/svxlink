/**
@file	 ModuleFrn.cpp
@brief   Free Radio Network (FRN) module
@author  sh123
@date	 2014-12-30

\verbatim
A module (plugin) for the multi purpose tranciever frontend system.
Copyright (C) 2004  Tobias Blomberg

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
#include <stdio.h>
#include <iostream>
#include <sstream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cctype>
#include <AsyncExec.h>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/
#include <AsyncConfig.h>
#include <AsyncAudioSplitter.h>
#include <AsyncAudioValve.h>
#include <AsyncAudioSelector.h>
#include <AsyncAudioFifo.h>
#include <AsyncAudioJitterFifo.h>
#include <AsyncAudioDecimator.h>
#include <AsyncAudioInterpolator.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/
#include <version/MODULE_FRN.h>
#include "ModuleFrn.h"
#include "multirate_filter_coeff.h"
#include "../../svxlink/SvxStats.h"


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/
using namespace std;
using namespace Async;


/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/


/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/


/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/


/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/


/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/


/****************************************************************************
 *
 * Pure C-functions
 *
 ****************************************************************************/
extern "C" {
  Module *module_init(void *dl_handle, Logic *logic, const char *cfg_name)
  {
    return new ModuleFrn(dl_handle, logic, cfg_name);
  }
} /* extern "C" */



/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/
ModuleFrn::ModuleFrn(void *dl_handle, Logic *logic, const string& cfg_name)
  : Module(dl_handle, logic, cfg_name)
  , qso(0)
  , audio_valve(0)
  , audio_splitter(0)
  , audio_selector(0)
  , audio_fifo(0)
  , aiorsctl_path("/usr/local/bin/aiorsctl")
  , run_cmd_secret("")
  , cmd_exec(0)
  , cmd_busy(false)
{
  cout << "\tModule Frn v" MODULE_FRN_VERSION " starting...\n";

} /* ModuleFrn */


ModuleFrn::~ModuleFrn(void)
{
  moduleCleanup();
} /* ~ModuleFrn */


/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/


/*
 *------------------------------------------------------------------------
 * Method:    
 * Purpose:   
 * Input:     
 * Output:    
 * Author:    
 * Created:   
 * Remarks:   
 * Bugs:      
 *------------------------------------------------------------------------
 */


/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/


/*
 *----------------------------------------------------------------------------
 * Method:    initialize
 * Purpose:   Called by the core system right after the object has been
 *    	      constructed. As little of the initialization should be done in
 *    	      the constructor. It's easier to handle errors here.
 * Input:     None
 * Output:    Return \em true on success or else \em false should be returned
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2005-08-28
 * Remarks:   The base class initialize method must be called from here.
 * Bugs:      
 *----------------------------------------------------------------------------
 */
bool ModuleFrn::initialize(void)
{
  if (!Module::initialize())
  {
    return false;
  }
 
  qso = new QsoFrn(this);
  qso->error.connect(
      mem_fun(*this, &ModuleFrn::onQsoError));
  qso->frnClientListReceived.connect(
      mem_fun(*this, &ModuleFrn::onFrnClientListReceived));
  qso->stateChange.connect(
      mem_fun(*this, &ModuleFrn::onQsoStateChange));
  qso->frnTxBytes.connect(
      mem_fun(*this, &ModuleFrn::onFrnTxBytes));
  qso->frnRxBytes.connect(
      mem_fun(*this, &ModuleFrn::onFrnRxBytes));

  // --- RunCmd -> aiorsctl bridge configuration ---
  std::string v;
  if (cfg().getValue(cfgName(), "AIORSCTL_PATH", v) && !v.empty())
  {
    aiorsctl_path = v;
  }
  v.clear();
  if (cfg().getValue(cfgName(), "RUN_CMD_SECRET", v))
  {
    run_cmd_secret = v;
  }

  qso->textMessageReceived.connect(
      mem_fun(*this, &ModuleFrn::onTextMessageReceived));


  // rig/mic -> frn
  audio_valve = new AudioValve;
  audio_splitter = new AudioSplitter;

  AudioSink::setHandler(audio_valve);
  audio_valve->registerSink(audio_splitter);
#if INTERNAL_SAMPLE_RATE == 16000
  AudioDecimator *down_sampler = new AudioDecimator(
      2, coeff_16_8, coeff_16_8_taps);
  audio_splitter->addSink(down_sampler, true);
  down_sampler->registerSink(qso);
#else
  audio_splitter->addSink(qso);
#endif

  // frn -> rig/speaker
  audio_selector = new AudioSelector;
  audio_fifo = new Async::AudioFifo(100 * 320 * 5);

#if INTERNAL_SAMPLE_RATE == 16000
  AudioInterpolator *up_sampler = new AudioInterpolator(
      2, coeff_16_8, coeff_16_8_taps);
  qso->registerSink(up_sampler, true);
  audio_selector->addSource(up_sampler);
  audio_selector->enableAutoSelect(up_sampler, 0);
#else
  audio_selector->addSource(qso);
  audio_selector->enableAutoSelect(qso, 0);
#endif
  audio_fifo->registerSource(audio_selector);
  AudioSource::setHandler(audio_fifo);

  if (!qso->initOk())
  {
    delete qso;
    cerr << "*** ERROR: Creation of Qso object failed\n";
    return false;
  }

  return true;
  
} /* initialize */


void ModuleFrn::moduleCleanup()
{
  AudioSource::clearHandler();
  audio_fifo->unregisterSource();

  audio_splitter->removeSink(qso);
  audio_valve->unregisterSink();
  AudioSink::clearHandler();

  delete qso;
  qso = 0;

  delete audio_fifo;
  audio_fifo = 0;

  delete audio_splitter;
  audio_splitter = 0;

  delete audio_valve;
  audio_valve = 0;

  delete audio_selector;
  audio_selector = 0;
}

/*
 *----------------------------------------------------------------------------
 * Method:    activateInit
 * Purpose:   Called by the core system when this module is activated.
 * Input:     None
 * Output:    None
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2004-03-07
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void ModuleFrn::activateInit(void)
{
    audio_valve->setOpen(true);
    qso->connect();
}


/*
 *----------------------------------------------------------------------------
 * Method:    deactivateCleanup
 * Purpose:   Called by the core system when this module is deactivated.
 * Input:     None
 * Output:    None
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2004-03-07
 * Remarks:   Do NOT call this function directly unless you really know what
 *    	      you are doing. Use Module::deactivate() instead.
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void ModuleFrn::deactivateCleanup(void)
{
  audio_valve->setOpen(true);
  qso->disconnect();
}


/*
 *----------------------------------------------------------------------------
 * Method:    dtmfDigitReceived
 * Purpose:   Called by the core system when a DTMF digit has been
 *    	      received. This function will only be called if the module
 *    	      is active.
 * Input:     digit   	- The DTMF digit received (0-9, A-D, *, #)
 *            duration	- The length in milliseconds of the received digit
 * Output:    Return true if the digit is handled or false if not
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2004-03-07
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
bool ModuleFrn::dtmfDigitReceived(char digit, int duration)
{
  cout << "DTMF digit received in module " << name() << ": " << digit << endl;

  return false;

} /* dtmfDigitReceived */


/*
 *----------------------------------------------------------------------------
 * Method:    dtmfCmdReceived
 * Purpose:   Called by the core system when a DTMF command has been
 *    	      received. A DTMF command consists of a string of digits ended
 *    	      with a number sign (#). The number sign is not included in the
 *    	      command string. This function will only be called if the module
 *    	      is active.
 * Input:     cmd - The received command.
 * Output:    None
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2004-03-07
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void ModuleFrn::dtmfCmdReceived(const string& cmd)
{
  cout << "DTMF command received in module " << name() << ": " << cmd << endl;

  if (cmd == "")
  {
    deactivateMe();
    return;
  }

  stringstream ss;

  switch (cmd[0])
  {
    case CMD_HELP:
      playHelpMsg();
      break;

    case CMD_COUNT_CLIENTS:
    {
      if (!validateCommand(cmd, 1))
        return;
      ss << "count_clients ";
      ss << qso->clientsCount();
      break;
    }

    case CMD_RF_DISABLE:
    {
      if (!validateCommand(cmd, 2))
        return;

      bool disable = (cmd[1] != '0');
      qso->setRfDisabled(disable);
      cout << "rf disable: " << disable << endl;
      ss << "rf_disable " <<  (qso->isRfDisabled() ? "1 " : "0 ")
         << (disable ? "1" : "0");
      break;
    }

    default:
      ss << "unknown_command " << cmd;
      break;
  }

  processEvent(ss.str());

} /* dtmfCmdReceived */


bool ModuleFrn::validateCommand(const string& cmd, size_t argc)
{
  if (cmd.size() == argc)
  {
    return true;
  } 
  else
  {
    stringstream ss;
    ss << "command_failed " << cmd;
    processEvent(ss.str());
    return false;
  }
} /* ModulrFrn::commandFailed */


#if 0
void ModuleFrn::dtmfCmdReceivedWhenIdle(const std::string &cmd)
{

} /* dtmfCmdReceivedWhenIdle */
#endif


/*
 *----------------------------------------------------------------------------
 * Method:    squelchOpen
 * Purpose:   Called by the core system when the squelch open or close.
 * Input:     is_open - Set to \em true if the squelch is open or \em false
 *    	      	      	if it's not.
 * Output:    None
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2005-08-28
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void ModuleFrn::squelchOpen(bool is_open)
{
  qso->squelchOpen(is_open);
}


/*
 *----------------------------------------------------------------------------
 * Method:    allMsgsWritten
 * Purpose:   Called by the core system when all announcement messages has
 *    	      been played. Note that this function also may be called even
 *    	      if it wasn't this module that initiated the message playing.
 * Input:     None
 * Output:    None
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2005-08-28
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void ModuleFrn::allMsgsWritten(void)
{

} /* allMsgsWritten */


/*
 *----------------------------------------------------------------------------
 * Method:    reportState
 * Purpose:   This function is called by the logic core when it wishes the
 *    	      module to report its state on the radio channel. Typically this
 *    	      is done when a manual identification has been triggered by the
 *    	      user by sending a "*".
 *    	      This function will only be called if this module is active.
 * Input:     None
 * Output:    None
 * Author:    Tobias Blomberg / SM0SVX
 * Created:   2005-08-28
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void ModuleFrn::reportState(void)
{
  stringstream ss;
  ss << "count_clients " << qso->clientsCount();
  processEvent(ss.str());
} /* reportState */


void ModuleFrn::onQsoError(void)
{
  cerr << "QSO errored, deactivating module" << endl;
  deactivateMe();
}



void ModuleFrn::onFrnClientListReceived(const FrnList& list)
{
  // Treat each list entry as an opaque user identifier/descriptor.
  // The FRN server/client implementations vary; we only need stable uniqueness.
  std::vector<std::string> v;
  v.reserve(list.size());
  for (const auto& s : list) v.push_back(s);
  SvxStats::instance().onFrnClientListUpdate(v);
}

void ModuleFrn::onQsoStateChange(QsoFrn::State st)
{
  // FRN TX is active in any of the TX audio states.
  const bool is_tx = (st == QsoFrn::STATE_TX_AUDIO ||
                      st == QsoFrn::STATE_TX_AUDIO_APPROVED ||
                      st == QsoFrn::STATE_TX_AUDIO_WAITING);
  const bool is_rx = (st == QsoFrn::STATE_RX_AUDIO);

  SvxStats::instance().onFrnTxState(is_tx);
  SvxStats::instance().onFrnRxState(is_rx);
}

void ModuleFrn::onFrnTxBytes(uint64_t bytes)
{
  SvxStats::instance().addFrnTxBytes(bytes);
}

void ModuleFrn::onFrnRxBytes(uint64_t bytes)
{
  SvxStats::instance().addFrnRxBytes(bytes);
}

static inline std::string trim_copy(const std::string& s)
{
  const char* ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

static inline std::string tolower_copy(const std::string& s)
{
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c){ return std::tolower(c); });
  return out;
}

bool ModuleFrn::parseAndAuthorizeCmd(const std::string& from_id,
                                     const std::string& msg,
                                     std::string& out_cmd,
                                     std::string& out_err)
{
  (void)from_id;
  std::string m = trim_copy(msg);
  std::string m_l = tolower_copy(m);

  const std::string prefix = "runcmd:";
  if (m_l.rfind(prefix, 0) != 0)
  {
    return false; // Not a RunCmd message
  }

  std::string rest = trim_copy(m.substr(prefix.size()));

  // Optional auth: RunCmd:<SECRET>: <cmd>  OR  RunCmd:<SECRET> <cmd>
  if (!run_cmd_secret.empty())
  {
    if (rest.rfind(run_cmd_secret + ":", 0) == 0)
    {
      rest = trim_copy(rest.substr(run_cmd_secret.size() + 1));
    }
    else if (rest.rfind(run_cmd_secret + " ", 0) == 0)
    {
      rest = trim_copy(rest.substr(run_cmd_secret.size() + 1));
    }
    else
    {
      out_err = "CmdReply: ERR: auth failed";
      return true; // It WAS RunCmd, but unauthorized
    }
  }

  rest = tolower_copy(trim_copy(rest));
  if (rest.empty())
  {
    out_err = "CmdReply: ERR: empty command";
    return true;
  }

  // Allowlist
  std::istringstream iss(rest);
  std::vector<std::string> tok;
  std::string t;
  while (iss >> t) tok.push_back(t);

  bool allowed = false;
  if (tok.size() == 3 && tok[0] == "get" && tok[1] == "psu" &&
      (tok[2] == "a" || tok[2] == "b" || tok[2] == "all"))
  {
    allowed = true;
  }
  else if (tok.size() == 3 && tok[0] == "get" && tok[1] == "temp" &&
           tok[2] == "all")
  {
    allowed = true;
  }
  else if (tok.size() == 2 && tok[0] == "get" && tok[1] == "stats")
  {
    allowed = true;
  }

  if (!allowed)
  {
    out_err = "CmdReply: ERR: command not allowed";
    return true;
  }

  out_cmd = rest;
  return true;
}

void ModuleFrn::sendCmdReplyChunked(const std::string& to_id,
                                    const std::string& reply)
{
  // Sanitize: remove CR/LF and other control chars (keep tab)
  std::string s = reply;
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
  s.erase(std::remove_if(s.begin(), s.end(),
                         [](unsigned char c){
                           return (c < 0x20 && c != '\t') || c == 0x7F;
                         }),
          s.end());

  const std::string base_prefix = "CmdReply: ";
  const std::string part_prefix_example = "CmdReply: [99/99] ";
  const size_t reserve = part_prefix_example.size();

  // If it fits in one TM, keep it simple (no numbering)
  if (base_prefix.size() + s.size() <= FRN_TM_MAX_CHARS)
  {
    qso->sendTextMessage(to_id, base_prefix + s);
    return;
  }

  // Chunked + numbered: CmdReply: [i/N] ...
  const size_t payload_max = FRN_TM_MAX_CHARS;
  const size_t chunk_size = (payload_max > reserve) ? (payload_max - reserve) : 50;

  std::vector<std::string> chunks;
  chunks.reserve((s.size() / chunk_size) + 1);

  for (size_t off = 0; off < s.size(); )
  {
    size_t take = std::min(chunk_size, s.size() - off);

    // Try to break on a space for readability
    if (off + take < s.size())
    {
      size_t last_space = s.rfind(' ', off + take);
      if (last_space != std::string::npos && last_space > off + 20)
        take = last_space - off;
    }

    chunks.push_back(s.substr(off, take));
    off += take;
    while (off < s.size() && s[off] == ' ') off++;
  }

  const size_t total = chunks.size();
  for (size_t i = 0; i < total; ++i)
  {
    std::ostringstream pfx;
    pfx << "CmdReply: [" << (i+1) << "/" << total << "] ";
    qso->sendTextMessage(to_id, pfx.str() + chunks[i]);
  }
}



void ModuleFrn::onTextMessageReceived(const std::string& from_id,
                                     const std::string& msg,
                                     const std::string& scope)
{
  std::string cmd;
  std::string err;

  bool is_runcmd = parseAndAuthorizeCmd(from_id, msg, cmd, err);
  if (!is_runcmd)
  {
    return; // ignore non-RunCmd messages
  }

  // Only accept private/direct messages. If someone tries RunCmd via broadcast,
  // log it once but do not execute and do not reply.
  if (scope != "P")
  {
    std::string m = trim_copy(msg);
    std::cout << "FRN RunCmd IGNORED (broadcast) from " << from_id
              << ": " << m << std::endl;
    SvxStats::instance().onCmdBroadcastAttempt();
    return;
  }

  if (!err.empty() && cmd.empty())
  {
    // RunCmd but rejected/unauthorized/invalid (no executable cmd)
    std::cout << "FRN RunCmd REJECTED from " << from_id << ": (" << err << ")"
              << std::endl;
    if (err.find("AUTH") != std::string::npos) SvxStats::instance().onCmdAuthFailed();
    else SvxStats::instance().onCmdRejected();
    // err already includes "CmdReply: ..." prefix for user-visible error
    qso->sendTextMessage(from_id, err);
    return;
  }

  // From here on, we have a validated command string in 'cmd'

  // Internal command (no aiorsctl): get stats
// NOTE: Keep this as a SINGLE FRN TM packet (no burst / chunking) because
// some FRN servers/clients will drop the TCP connection if we emit multiple
// <TM> packets back-to-back.
if (cmd == "get stats")
{
  SvxStats::instance().onCmdAccepted();

  std::vector<std::string> groups = SvxStats::instance().formatStatsGroups();
  if (groups.empty())
    groups.push_back("STATS (empty)");

  // Numbered replies: CmdReply: [i/N] ...
  const size_t n = groups.size();
  const std::string base_prefix = "CmdReply: ";
  for (size_t i = 0; i < n; ++i)
  {
    std::ostringstream pfx;
    pfx << base_prefix << "[" << (i+1) << "/" << n << "] ";

    std::string line = groups[i];
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

    // Hard clamp to FRN_TM_MAX_CHARS
    if (pfx.str().size() < FRN_TM_MAX_CHARS)
    {
      const size_t max_payload = FRN_TM_MAX_CHARS - pfx.str().size();
      if (line.size() > max_payload && max_payload >= 3)
      {
        line.resize(max_payload - 3);
        line += "...";
      }
      else if (line.size() > max_payload)
      {
        line.resize(max_payload);
      }
    }
    else
    {
      line.clear();
    }

    qso->sendTextMessage(from_id, pfx.str() + line);
  }
  return;
}

  if (cmd_busy)
  {
    std::cout << "FRN RunCmd BUSY from " << from_id << ": " << cmd << std::endl;
    SvxStats::instance().onCmdRejected();
    qso->sendTextMessage(from_id, "CmdReply: ERR: busy");
    return;
  }

  cmd_busy = true;
  cmd_from_id = from_id;
  cmd_stdout.clear();
  cmd_stderr.clear();

  std::cout << "FRN RunCmd ACCEPTED from " << from_id << ": " << cmd << std::endl;
  SvxStats::instance().onCmdAccepted();
  cmd_start_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();

  std::string cmdline = aiorsctl_path + " " + cmd;
  cmd_exec = new Async::Exec(cmdline);
  cmd_exec->setTimeout(3); // seconds

  cmd_exec->stdoutData.connect(
      [this](const char* buf, int cnt)
      {
        cmd_stdout.append(buf, cnt);
      });

  cmd_exec->stderrData.connect(
      [this](const char* buf, int cnt)
      {
        cmd_stderr.append(buf, cnt);
      });

  cmd_exec->exited.connect(
      [this](void)
      {
        std::string out = trim_copy(cmd_stdout);
        std::string errout = trim_copy(cmd_stderr);
        std::string reply = out.empty() ? errout : out;
        if (reply.empty())
        {
          reply = "OK";
        }

        sendCmdReplyChunked(cmd_from_id, reply);
        uint64_t end_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count();
        if (end_ms >= cmd_start_ms)
          SvxStats::instance().onCmdExecTimeMs((uint32_t)(end_ms - cmd_start_ms));

        delete cmd_exec;
        cmd_exec = 0;
        cmd_busy = false;
      });

  cmd_exec->run();
}

