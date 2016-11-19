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

#ifndef __p44sbbd__sbbcomm__
#define __p44sbbd__sbbcomm__

#include "p44utils_common.hpp"

#include "serialqueue.hpp"
#include "digitalio.hpp"

using namespace std;

namespace p44 {


  class SbbComm;
  class SbbRow;

  typedef enum {
    moduletype_alphanum,
    moduletype_hour,
    moduletype_minute,
    moduletype_40,
    moduletype_62
  } SbbModuleType;


//  typedef struct {
//    SbbModuleType type; // type of module (determines conversions)
//    uint8_t val; // value (index, char)
//  } SbbModulePos;
//
//  const int numlines = 2; // each side of the unit is a line
//  const int numchars = 6; // 6 "chars" per "line"
//
//  typedef struct {
//    SbbModulePos chars[numchars];
//  } SbbModuleLine;
//

  typedef boost::function<void (const string &aResponse, ErrorPtr aError)> SBBResultCB;


  typedef boost::intrusive_ptr<SbbComm> SbbCommPtr;
  class SbbComm : public SerialOperationQueue
  {
    typedef SerialOperationQueue inherited;

    DigitalIoPtr txEnable;
    DigitalIoPtr rxEnable;
    enum {
      txEnable_none,
      txEnable_io,
      txEnable_dtr,
      txEnable_rts
    } txEnableMode;
    MLMicroSeconds txOffDelay;
    long txOffTicket;

  public:

    SbbComm(MainLoop &aMainLoop);
    virtual ~SbbComm();

    /// set the connection parameters to connect to the SBB RS485 bus
    /// @param aConnectionSpec serial device path (/dev/...) or host name/address[:port] (1.2.3.4 or xxx.yy)
    /// @param aDefaultPort default port number for TCP connection (irrelevant for direct serial device connection)
    void setConnectionSpecification(const char *aConnectionSpec, uint16_t aDefaultPort);

    /// set the RS485 driver control lines
    /// @param aTxEnablePinSpec the digital output line to be used for enabling RS485 transmitter
    /// @param aRxEnablePinSpec the digital output line to be used for enabling RS485 receiver
    /// @param aOffDelay how long to keep TX enabled after enableSending(false)
    void setRS485DriverControl(const char *aTxEnablePinSpec, const char *aRxEnablePinSpec, MLMicroSeconds aOffDelay);

    /// send raw command (starting with BREAK)
    void sendRawCommand(const string aCommand, size_t aExpectedBytes, SBBResultCB aResultCB, MLMicroSeconds aInitiationDelay=0.2*Second);

    /// set the value to display in a module
    /// @param aModuleAddr the module address
    /// @param aType the module type, controls value->position transformation
    /// @param aValue the value to show.
    void setModuleValue(uint8_t aModuleAddr, SbbModuleType aType, uint8_t aValue);

  protected:

    /// called to process extra bytes after all pending operations have processed their bytes
    virtual ssize_t acceptExtraBytes(size_t aNumBytes, uint8_t *aBytes);

    /// RS485 driver control
    /// @param aEnable set to enable sending, clear after sending
    void enableSending(bool aEnable);

  private:

    /// special transmitter
    size_t sbbTransmitter(size_t aNumBytes, const uint8_t *aBytes);

    void sbbCommandComplete(SBBResultCB aStatusCB, SerialOperationPtr aSerialOperation, ErrorPtr aError);
    void enableSendingImmediate(bool aEnable);

  };



} // namespace p44

#endif /* defined(__p44sbbd__sbbcomm__) */
