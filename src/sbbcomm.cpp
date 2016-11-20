//
//  Copyright (c) 2015 plan44.ch / Lukas Zeller, Zurich, Switzerland
//
//  Author: Lukas Zeller <luz@plan44.ch>
//
//  This file is part of p44ayabd.
//
//  p44ayabd is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  p44ayabd is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with p44ayabd. If not, see <http://www.gnu.org/licenses/>.
//

#include "sbbcomm.hpp"

#include "consolekey.hpp"
#include "application.hpp"

using namespace p44;

#define SBB_COMMPARAMS "19200,8,N,2"


// SBB RS485 protocol

#define SBB_SYNCBYTE 0xFF // all commands start with this

#define SBB_CMD_SETPOS 0xC0 // set position
#define SBB_CMD_GETPOS 0xD0 // get position
#define SBB_CMD_GETSERIAL 0xDF // get serial number



#pragma mark - SbbComm

SbbComm::SbbComm(MainLoop &aMainLoop) :
	inherited(aMainLoop),
  txOffDelay(0),
  txEnableMode(txEnable_none),
  txOffTicket(0)
{
}


SbbComm::~SbbComm()
{
}


void SbbComm::setConnectionSpecification(const char *aConnectionSpec, uint16_t aDefaultPort)
{
  LOG(LOG_DEBUG, "SbbComm::setConnectionSpecification: %s", aConnectionSpec);
  if (strcmp(aConnectionSpec,"simulation")==0) {
    // simulation mode
  }
  else {
    serialComm->setConnectionSpecification(aConnectionSpec, aDefaultPort, SBB_COMMPARAMS);
    // we need a non-standard transmitter
    setTransmitter(boost::bind(&SbbComm::sbbTransmitter, this, _1, _2));
    // open connection so we can receive from start
    if (serialComm->requestConnection()) {
      serialComm->setRTS(false); // not sending
    }
//    // set accept buffer for re-assembling messages before processing
//    setAcceptBuffer(100); // we don't know yet how long SBB messages can get
  }
}


void SbbComm::setRS485DriverControl(const char *aTxEnablePinSpec, const char *aRxEnablePinSpec, MLMicroSeconds aOffDelay)
{
  txOffDelay = aOffDelay;
  if (strcmp(aTxEnablePinSpec, "DTR")==0) {
    txEnableMode = txEnable_dtr;
  }
  else if (strcmp(aTxEnablePinSpec, "RTS")==0) {
    txEnableMode = txEnable_rts;
  }
  else {
    // digital I/O line
    txEnableMode = txEnable_io;
    txEnable = DigitalIoPtr(new DigitalIo(aTxEnablePinSpec, true, false));
    rxEnable = DigitalIoPtr(new DigitalIo(aRxEnablePinSpec, true, true));
  }
}


void SbbComm::enableSendingImmediate(bool aEnable)
{
  switch(txEnableMode) {
    case txEnable_dtr:
      serialComm->setDTR(aEnable);
      return;
    case txEnable_rts:
      serialComm->setRTS(aEnable);
      return;
    case txEnable_io:
      rxEnable->set(!aEnable);
      txEnable->set(aEnable);
      return;
    default:
      return; // NOP
  }
}


void SbbComm::enableSending(bool aEnable)
{
  MainLoop::currentMainLoop().cancelExecutionTicket(txOffTicket);
  if (aEnable || txOffDelay==0) {
    enableSendingImmediate(aEnable);
  }
  else {
    txOffTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&SbbComm::enableSendingImmediate, this, aEnable), txOffDelay);
  }
}




size_t SbbComm::sbbTransmitter(size_t aNumBytes, const uint8_t *aBytes)
{
  ssize_t res = 0;
  ErrorPtr err = serialComm->establishConnection();
  if (Error::isOK(err)) {
    if (LOGENABLED(LOG_NOTICE)) {
      string m;
      for (size_t i=0; i<aNumBytes; i++) {
        string_format_append(m, " %02X", aBytes[i]);
      }
      LOG(LOG_NOTICE, "transmitting bytes:%s", m.c_str());
    }
    // enable sending
    enableSending(true);
    // send break
    serialComm->sendBreak();
    // now let standard transmitter do the rest
    res = standardTransmitter(aNumBytes, aBytes);
    // disable sending
    enableSending(false);
  }
  else {
    LOG(LOG_DEBUG, "SbbComm::sbbTransmitter error - connection could not be established!");
  }
  return res;
}



void SbbComm::sendRawCommand(const string aCommand, size_t aExpectedBytes, SBBResultCB aResultCB, MLMicroSeconds aInitiationDelay)
{
  LOG(LOG_INFO, "Posting command");
  SerialOperationSendPtr req = SerialOperationSendPtr(new SerialOperationSend);
  req->setDataSize(aCommand.size());
  req->appendData(aCommand.size(), (uint8_t *)aCommand.c_str());
  req->setInitiationDelay(aInitiationDelay);
  if (aExpectedBytes>0) {
    // we expect some answer bytes
    SerialOperationReceivePtr resp = SerialOperationReceivePtr(new SerialOperationReceive);
    resp->setCompletionCallback(boost::bind(&SbbComm::sbbCommandComplete, this, aResultCB, resp, _1));
    resp->setExpectedBytes(aExpectedBytes);
    resp->setTimeout(2*Second);
    req->setChainedOperation(resp);
  }
  else {
    req->setCompletionCallback(boost::bind(&SbbComm::sbbCommandComplete, this, aResultCB, SerialOperationPtr(), _1));
  }
  queueSerialOperation(req);
  // process operations
  processOperations();
}


void SbbComm::sbbCommandComplete(SBBResultCB aResultCB, SerialOperationPtr aSerialOperation, ErrorPtr aError)
{
  LOG(LOG_INFO, "Command complete");
  string result;
  if (Error::isOK(aError)) {
    SerialOperationReceivePtr resp = boost::dynamic_pointer_cast<SerialOperationReceive>(aSerialOperation);
    if (resp) {
      result.assign((char *)resp->getDataP(), resp->getDataSize());
    }
  }
  if (aResultCB) aResultCB(result, aError);
}


void SbbComm::setModuleValue(uint8_t aModuleAddr, SbbModuleType aType, uint8_t aValue)
{
  uint8_t pos;
  switch (aType) {
    case moduletype_alphanum :
      // use characters. Order in module is
      // ABCDEFGHIJKLMNOPQRSTUVWXYZ/-1234567890.<space>
      // 0123456789012345678901234567890123456789
      // 0         1         2         3        3
      if (aValue>='A' && aValue<='Z') {
        pos = aValue-'A';
      }
      else if (aValue=='/') {
        pos = 26;
      }
      else if (aValue=='-') {
        pos = 27;
      }
      else if (aValue>='1' && aValue<='9') {
        pos = aValue-'1'+28;
      }
      else if (aValue=='.') {
        pos = 38;
      }
      else {
        // everything else: space
        pos = 39;
      }
      break;
    case moduletype_hour :
      // hours 0..23, >23 = space
      pos = aValue>23 ? 24 : aValue;
      break;
    case moduletype_minute :
      // 0..59, >59 = space
      // pos 0..28 are minutes 31..59
      // pos 29 is space
      // pos 30..60 are minutes 00..30
      // pos 61 is space
      pos = aValue>59 ? 29 : (aValue<31 ? 30+aValue : aValue-31);
      break;
    case moduletype_40 :
    case moduletype_62 :
      // just use as-is
      pos = aValue;
      break;
  }
  string poscmd = "\xFF\xC0";
  poscmd += (char)aModuleAddr;
  poscmd += (char)pos;
  sendRawCommand(poscmd, 0, NULL);
}





ssize_t SbbComm::acceptExtraBytes(size_t aNumBytes, uint8_t *aBytes)
{
  // got bytes with no command expecting them in particular
  if (LOGENABLED(LOG_INFO)) {
    string m;
    for (size_t i=0; i<aNumBytes; i++) {
      string_format_append(m, " %02X", aBytes[i]);
    }
    LOG(LOG_NOTICE, "received extra bytes:%s", m.c_str());
  }
  return (ssize_t)aNumBytes;
}


