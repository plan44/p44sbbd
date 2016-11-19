//
//  Copyright (c) 2016 plan44.ch / Lukas Zeller, Zurich, Switzerland
//
//  Author: Lukas Zeller <luz@plan44.ch>
//
//  This file is part of p44sbbd.
//
//  p44sbbd is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  p44sbbd is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with p44ayabd. If not, see <http://www.gnu.org/licenses/>.
//

#include "application.hpp"

#include "sbbcomm.hpp"
#include "jsoncomm.hpp"
#include "utils.hpp"

using namespace p44;

#define DEFAULT_LOGLEVEL LOG_NOTICE
#define DEFAULT_STATE_DIR "/tmp"

#define MAINLOOP_CYCLE_TIME_uS 33333 // 33mS


typedef struct {
  uint8_t cmd;
  size_t answerbytes;
  size_t parambytes;
  const char *name;
  const char *desc;
} SBBCmdDesc;

//  Kommando    Schreiben  Lesen
//  ----------------------------
//  DISP/RDB    C0         D0
//  STAT        C1         D1
//  RESET/VER   C4         D4
//  ZERO        C5
//  STEP        C6
//  PULSE       C7
//  TEST        C8
//  CTRL        C9         D9
//  NULL/POS    CA         DA
//  WIN         CB         DB
//  CALB        CC         DC
//  TYPE        CD         DD
//  ADDR        CE         DE
//  SNBR        CF         DF

const SBBCmdDesc sbbCmds[] = {
  { 0xC0, 0, 1, "DISP", "Set Position" },
  { 0xD0, 1, 0, "RDB",  "Readback Position" },
  { 0xC1, 0, 0, "STAT",  "Status???" },
  { 0xD1, 1, 1, "STAT",  "Status???" },
  { 0xC4, 0, 0, "RESET",  "Reset???" },
  { 0xD4, 2, 2, "VER",  "Get Version" },
  { 0xC5, 0, 0, "ZERO",  "Zero???" },
  { 0xC6, 0, 0, "STEP",  "Step???" },
  { 0xC7, 0, 0, "PULSE",  "Pulse???" },
  { 0xC8, 0, 0, "TEST",  "Test???" },
  { 0xC9, 0, 0, "CTRL",  "Control???" },
  { 0xD9, 1, 0, "CTRL",  "Control???" },
  { 0xCA, 0, 0, "NULL",  "Null???" },
  { 0xDA, 2, 0, "POS",  "Position???" },
  { 0xCB, 0, 1, "WIN",  "Win???" },
  { 0xDB, 1, 0, "WIN",  "Win???" },
  { 0xCC, 0, 0, "CALB",  "Calibrate???" },
  { 0xDC, 0, 0, "CALB",  "Calibrate???NoAnswer?" },
  { 0xCD, 0, 0, "TYPE",  "Set Type???" },
  { 0xDD, 1, 0, "TYPE",  "Get Type???" },
  { 0xCE, 0, 1, "ADDR",  "Set Module Address" },
  { 0xDE, 1, 0, "ADDR",  "Get Module Address" },
  { 0xCF, 0, 0, "SNBR",  "Set Serial???" },
  { 0xDF, 4, 0, "SNBR",  "Get Serial Number" },
  { 0, 0, 0, NULL, NULL }
};


static const char *weekdays[7] = {
  "MO",
  "DI",
  "MI",
  "DO",
  "FR",
  "SA",
  "SO"
};



class P44sbbd : public CmdLineApp
{
  typedef CmdLineApp inherited;

  SbbCommPtr sbbComm;

  bool apiMode; ///< set if in API mode (means working as daemon, not quitting when job is done)
  // API Server
  SocketCommPtr apiServer;

  string statedir;

  long initiateTicket;

  // clock
  bool clockEnabled;
  int hourmodule;
  int minutemodule;
  int weekday1module;
  int weekday2module;
  long clockTicket;

public:

  P44sbbd() :
    apiMode(false),
    initiateTicket(0),
    clockEnabled(false),
    hourmodule(-1),
    minutemodule(-1),
    weekday1module(-1),
    weekday2module(-1),
    clockTicket(0)
  {
  };


  virtual int main(int argc, char **argv)
  {
    const char *usageText =
      "Usage: %1$s [options]\n";
    const CmdLineOptionDescriptor options[] = {
      { 'l', "loglevel",        true,  "level;set max level of log message detail to show on stderr" },
      { 'W', "jsonapiport",     true,  "port;server port number for JSON API" },
      { 0  , "jsonapinonlocal", false, "allow connection to JSON API from non-local clients" },
      { 0  , "rs485connection", true,  "serial_if;RS485 serial interface where display is connected (/device or IP:port)" },
      { 0  , "rs485txenable",   true,  "pinspec;a digital output pin specification for TX driver enable or DTR or RTS" },
      { 0  , "rs485txoffdelay", true,  "delay;time to keep tx enabled after sending [ms], defaults to 0" },
      { 0  , "rs485rxenable",   true,  "pinspec;a digital output pin specification for RX driver enable" },
      { 0  , "timedisplay",     true,  "hourmodule,minutemodule;module addresses to be used for time display" },
      { 0  , "weekdaydisplay",  true,  "firstchar[,secondchar];module addresses to be used for weekday display" },
      { 0  , "statedir",        true,  "path;writable directory where to store state information. Defaults to " DEFAULT_STATE_DIR },
      { 'h', "help",            false, "show this text" },
      { 0, NULL } // list terminator
    };

    // parse the command line, exits when syntax errors occur
    setCommandDescriptors(usageText, options);
    parseCommandLine(argc, argv);

    if (numOptions()<1) {
      // show usage
      showUsage();
      terminateApp(EXIT_SUCCESS);
    }

    // log level?
    int loglevel = DEFAULT_LOGLEVEL;
    getIntOption("loglevel", loglevel);
    SETLOGLEVEL(loglevel);
    SETERRLEVEL(loglevel, false); // all diagnostics go to stderr

    // state dir
    statedir = DEFAULT_STATE_DIR;
    getStringOption("statedir", statedir);

    // app now ready to run
    return run();
  }


  virtual void initialize()
  {
    ErrorPtr err;

    // get AYAB connection
    // - set interface
    string s;
    if (getStringOption("rs485connection", s)) {
      sbbComm = SbbCommPtr(new SbbComm(MainLoop::currentMainLoop()));
      sbbComm->setConnectionSpecification(s.c_str(), 2109);
      string tx,rx;
      int txoffdelay = 0;
      getStringOption("rs485txenable", tx);
      getStringOption("rs485rxenable", rx);
      getIntOption("rs485txoffdelay", txoffdelay);
      sbbComm->setRS485DriverControl(tx.c_str(), rx.c_str(), txoffdelay*MilliSecond);
    }
    else {
      terminateAppWith(TextError::err("no RS485 connection specified"));
      return;
    }
    // - start API server and wait for things to happen
    string apiport;
    if (getStringOption("jsonapiport", apiport)) {
      apiServer = SocketCommPtr(new SocketComm(MainLoop::currentMainLoop()));
      apiServer->setConnectionParams(NULL, apiport.c_str(), SOCK_STREAM, AF_INET);
      apiServer->setAllowNonlocalConnections(getOption("jsonapinonlocal"));
      apiServer->startServer(boost::bind(&P44sbbd::apiConnectionHandler, this, _1), 3);
    }
    // - check for clock
    if (getStringOption("timedisplay", s)) {
      sscanf(s.c_str(), "%d,%d", &hourmodule, &minutemodule);
      clockEnabled = true;
    }
    if (getStringOption("weekdaydisplay", s)) {
      sscanf(s.c_str(), "%d,%d", &weekday1module, &weekday2module);
      clockEnabled = true;
    }
    if (clockEnabled) {
      // schedule update
      clockTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&P44sbbd::clockUpdate, this));
    }
//    // start status polling
//    statusPoll();
  };


  void cleanup(int aExitCode)
  {
    // clean up
  }



  void clockUpdate()
  {
    if (clockEnabled) {
      struct tm t;
      time_t tim = time(NULL);
      localtime_r(&tim, &t);
      // update clock display
      if (hourmodule>=0) {
        sbbComm->setModuleValue(hourmodule, moduletype_hour, t.tm_hour);
      }
      if (minutemodule>=0) {
        sbbComm->setModuleValue(minutemodule, moduletype_minute, t.tm_min);
      }
      if (weekday1module>=0) {
        // need weekday
        sbbComm->setModuleValue(weekday1module, moduletype_alphanum, weekdays[t.tm_wday][0]);
        if (weekday2module) {
          sbbComm->setModuleValue(weekday2module, moduletype_alphanum, weekdays[t.tm_wday][1]);
        }
      }
      // schedule next update
      clockTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&P44sbbd::clockUpdate, this), (60-t.tm_sec)*Second);
    }
  }




  SocketCommPtr apiConnectionHandler(SocketCommPtr aServerSocketComm)
  {
    JsonCommPtr conn = JsonCommPtr(new JsonComm(MainLoop::currentMainLoop()));
    conn->setMessageHandler(boost::bind(&P44sbbd::apiRequestHandler, this, conn, _1, _2));
    conn->setClearHandlersAtClose(); // close must break retain cycles so this object won't cause a mem leak
    return conn;
  }


  void apiRequestHandler(JsonCommPtr aConnection, ErrorPtr aError, JsonObjectPtr aRequest)
  {
    ErrorPtr err;
    JsonObjectPtr answer = JsonObject::newObj();
    // Decode mg44-style request (HTTP wrapped in JSON)
    if (Error::isOK(aError)) {
      LOG(LOG_INFO,"API request: %s", aRequest->c_strValue());
      JsonObjectPtr o;
      o = aRequest->get("method");
      if (o) {
        string method = o->stringValue();
        string uri;
        o = aRequest->get("uri");
        if (o) uri = o->stringValue();
        JsonObjectPtr data;
        bool action = (method!="GET");
        if (action) {
          data = aRequest->get("data");
        }
        else {
          data = aRequest->get("uri_params");
          if (data) action = true; // GET, but with query_params: treat like PUT/POST with data
        }
        // request elements now: uri and data
        JsonObjectPtr r = processRequest(uri, data, action);
        if (r) answer->add("result", r);
      }
    }
    else {
      LOG(LOG_ERR,"Invalid JSON request");
      answer->add("Error", JsonObject::newString(aError->description()));
    }
    LOG(LOG_INFO,"API answer: %s", answer->c_strValue());
    err = aConnection->sendMessage(answer);
    aConnection->closeAfterSend();
  }


  void statusPoll()
  {
    string statuscmd = "\xFF\xD9";
    statuscmd += (char)55;
    sbbComm->sendRawCommand(statuscmd, 1, boost::bind(&P44sbbd::statusAnswer, this, _1, _2));
  }


  void statusAnswer(const string &aAnswer, ErrorPtr aError)
  {
    if (aAnswer.size()>0) {
      LOG(LOG_NOTICE, "CTRL poll answer = %02X", aAnswer[0]);
    }
    MainLoop::currentMainLoop().executeOnce(boost::bind(&P44sbbd::statusPoll,this), 0.2*Second);
  }



  void setPosition(int aModuleAddr, int aPosition)
  {
    // create command
    string poscmd = "\xFF\xC0";
    poscmd += (char)aModuleAddr;
    poscmd += (char)aPosition;
    sbbComm->sendRawCommand(poscmd, 0, NULL);
  }


  void getInfo(int aModuleAddr)
  {
    LOG(LOG_NOTICE, "\nModule 0x%02X/%d", aModuleAddr, aModuleAddr);
    for (int i=0; sbbCmds[i].cmd!=0; i++) {
      if (sbbCmds[i].answerbytes>0) {
        string infocmd = "\xFF";
        infocmd += (char)sbbCmds[i].cmd;
        infocmd += (char)aModuleAddr;
        sbbComm->sendRawCommand(infocmd, sbbCmds[i].answerbytes, boost::bind(&P44sbbd::infoAnswer, this, i, _1, _2));
      }
    }
  }


  void infoAnswer(int aCmdIndex, const string &aAnswer, ErrorPtr aError)
  {
    if (Error::isOK(aError)) {
      LOG(LOG_NOTICE, "- %02X : %-4s -> (%lu) %s (%s)", sbbCmds[aCmdIndex].cmd, sbbCmds[aCmdIndex].name, aAnswer.size(), binaryToHexString(aAnswer, ' ').c_str(), sbbCmds[aCmdIndex].desc);
    }
    else {
      LOG(LOG_NOTICE, "- %02X : %-4s -> Error: %s", sbbCmds[aCmdIndex].cmd, sbbCmds[aCmdIndex].name, aError->description().c_str());
    }
  }


  JsonObjectPtr processRequest(string aUri, JsonObjectPtr aData, bool aIsAction)
  {
    ErrorPtr err;
    JsonObjectPtr o;
    if (aUri=="/interface") {
      if (aIsAction) {
        if (aData->get("sendbytes", o)) {
          if (o->isType(json_type_string)) {
            // hex string of bytes
            string bytes = hexToBinaryString(o->stringValue().c_str());
            sbbComm->sendRawCommand(bytes, 0, NULL);
          }
          else if (o->isType(json_type_array)) {
            // array of bytes
            size_t nb = o->arrayLength();
            string bytes;
            for (int i=0; i<nb; i++) {
              bytes[i] = (uint8_t)(o->arrayGet(i)->int32Value());
            }
            sbbComm->sendRawCommand(bytes, 0, NULL);
          }
        }
      }
    }
    else if (aUri=="/module") {
      if (aIsAction) {
        if (aData->get("addr", o)) {
          int moduleAddr = o->int32Value();
          if (aData->get("pos", o)) {
            int position = o->int32Value();
            setPosition(moduleAddr, position);
          }
          else if (aData->get("info")) {
            // create query commands
            getInfo(moduleAddr);
          }
        }
      }
    }
    else {
      err = WebError::webErr(500, "Unknown URI");
    }
    // return error or ok
    if (Error::isOK(err))
      return JsonObjectPtr(); // ok
    else {
      JsonObjectPtr errorJson = JsonObject::newObj();
      errorJson->add("error", JsonObject::newString(err->description()));
      return errorJson;
    }
  }

};


int main(int argc, char **argv)
{
  // prevent debug output before application.main scans command line
  SETLOGLEVEL(LOG_EMERG);
  SETERRLEVEL(LOG_EMERG, false); // messages, if any, go to stderr
  // create the mainloop
  MainLoop::currentMainLoop().setLoopCycleTime(MAINLOOP_CYCLE_TIME_uS);
  // create app with current mainloop
  static P44sbbd application;
  // pass control
  return application.main(argc, argv);
}


