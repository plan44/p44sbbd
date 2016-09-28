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



class P44sbbd : public CmdLineApp
{
  typedef CmdLineApp inherited;

  SbbCommPtr sbbComm;

  bool apiMode; ///< set if in API mode (means working as daemon, not quitting when job is done)
  // API Server
  SocketCommPtr apiServer;

  string statedir;

  long initiateTicket;

public:

  P44sbbd() :
    apiMode(false),
    initiateTicket(0)
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
    string connstring;
    if (getStringOption("rs485connection", connstring)) {
      sbbComm = SbbCommPtr(new SbbComm(MainLoop::currentMainLoop()));
      sbbComm->setConnectionSpecification(connstring.c_str(), 2109);
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
  };


  void cleanup(int aExitCode)
  {
    // clean up
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
            sbbComm->sendRawCommand(bytes.length(), (uint8_t *)bytes.c_str(), NULL);
          }
          else if (o->isType(json_type_array)) {
            // array of bytes
            size_t nb = o->arrayLength();
            uint8_t *bytes = new uint8_t[nb];
            for (int i=0; i<nb; i++) {
              bytes[i] = (uint8_t)(o->arrayGet(i)->int32Value());
            }
            sbbComm->sendRawCommand(nb, bytes, NULL);
            delete[] bytes;
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
            // create command
            uint8_t poscmd[4];
            poscmd[0]=0xFF;
            poscmd[1]=0xC0;
            poscmd[2]=moduleAddr;
            poscmd[3]=position;
            sbbComm->sendRawCommand(sizeof(poscmd), poscmd, NULL);
          }
          else if (aData->get("info")) {
            // create query commands
            for (int i=0; i<16; i++) {
              if (i!=2 && i!=3 && (i<5 || i>8)) {
                uint8_t infocmd[3];
                infocmd[0]=0xFF;
                infocmd[1]=0xD0+i;
                infocmd[2]=moduleAddr;
                sbbComm->sendRawCommand(sizeof(infocmd), infocmd, NULL);
              }
            }
          }
        }
      }
    }
    else if (aUri=="/scan") {
      if (aIsAction) {
        for (int i=0; i<256; i++) {
          // create command
          uint8_t poscmd[5];
          poscmd[0]=0xFF;
          poscmd[1]=0xC0;
          poscmd[3]=0x51;
          poscmd[2]=i;
          poscmd[4]=21;
          sbbComm->sendRawCommand(sizeof(poscmd), poscmd, NULL);
        }
      }
    }
//    else if (aUri=="/machine") {
//      if (aIsAction) {
//        if (aData->get("restart", o)) {
//          if (o->boolValue()) {
//            restartAyab(true);
//          }
//        }
//        else if (aData->get("setWidth", o)) {
//          err = patternQueue->setWidth(o->int32Value());
//          patternQueue->saveState(statedir.c_str(), false);
//          // also needs restart
//          restartAyab(true);
//        }
//        else {
//          err = WebError::err(500, "Unknown action for /machine");
//        }
//      }
//      else {
//        o = JsonObject::newObj();
//        o->add("status", JsonObject::newInt32(ayabComm->getStatus()));
//        return o;
//      }
//    }
//    else if (aUri=="/queue") {
//      if (aIsAction) {
//        // check action to execute on queue
//        if (aData->get("addFile", o)) {
//          bool restartKnitting = patternQueue->endOfPattern(); // if we've been at the end of the pattern, we'll need to restart after loading new pattern
//          JsonObjectPtr p = aData->get("webURL");
//          err = patternQueue->addFile(o->stringValue(), p->stringValue());
//          patternQueue->saveState(statedir.c_str(), false);
//          if (Error::isOK(err)) {
//            // restart needed?
//            if (restartKnitting) {
//              MainLoop::currentMainLoop().cancelExecutionTicket(initiateTicket);
//              initiateTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&P44ayabd::initiateKnitting, this), 1*Second);
//            }
//          }
//        }
//        else if (aData->get("removeFile", o)) {
//          bool withDelete = false;
//          JsonObjectPtr del;
//          if (aData->get("delete", del)) {
//            withDelete = del->boolValue();
//          }
//          err = patternQueue->removeFile(o->int32Value(), withDelete);
//          patternQueue->saveState(statedir.c_str(), false);
//        }
//        else {
//          err = WebError::err(500, "Unknown action for /queue");
//        }
//      }
//      else {
//        // just GET - return queue
//        return patternQueue->queueStateJSON();
//      }
//    }
//    else if (aUri=="/cursor") {
//      if (aIsAction) {
//        // check action to execute on cursor
//        if (aData->get("setPosition", o)) {
//          bool beginningOfEntry = false;
//          JsonObjectPtr b;
//          if (aData->get("boundary", b)) {
//            beginningOfEntry = b->boolValue();
//          }
//          patternQueue->moveCursor(o->int32Value(), false, beginningOfEntry);
//          patternQueue->saveState(statedir.c_str(), false);
//        }
//      }
//      else {
//        // just return current cursor position
//        return patternQueue->cursorStateJSON();
//      }
//    }
    else {
      err = WebError::err(500, "Unknown URI");
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


