// Copyright (c) 2012, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "SPIFlash.h"
#include "Signal.h"
#include "RunnableQueue.h"
#include "PortInterface.h"
#include "Runnable.h"
#include "PeripheralDescriptor.h"
#include "Peripheral.h"
#include "PortConnectionManager.h"
#include "PortSignalTracker.h"
#include "Property.h"
#include "SystemState.h"
#include "Node.h"
#include "Core.h"
#include "PortHandleClockProxy.h"
#include <fstream>
#include <iostream>
#include <cstdlib>

using namespace axe;

class SPIFlash : public Peripheral {
  void seeSCLKChange(const Signal &value, ticks_t time);
  void seeSSChange(const Signal &value, ticks_t time);
  enum State {
    WAIT_FOR_CMD,
    WAIT_FOR_ADDRESS,
    READ,
    UNKNOWN_CMD
  };
  State state;
  unsigned char *mem;
  unsigned memSize;
  PortInterface *MISO;
  PortSignalTracker MOSITracker;
  PortInterfaceMemberFuncDelegate<SPIFlash> SCLKProxy;
  PortInterfaceMemberFuncDelegate<SPIFlash> SSProxy;
  PortHandleClockProxy SCLKHandleClock;
  PortHandleClockProxy SSHandleClock;
  unsigned MISOValue;
  unsigned SCLKValue;
  unsigned SSValue;
  uint8_t receiveReg;
  unsigned receivedBits;
  unsigned receivedAddressBytes;
  uint8_t sendReg;
  unsigned sendBitsRemaining;
  uint32_t readAddress;

  void reset();
public:
  SPIFlash(RunnableQueue &scheduler, PortConnectionWrapper MISO,
           PortConnectionWrapper MOSI, PortConnectionWrapper SCLK,
           PortConnectionWrapper SS);
  ~SPIFlash();
  void openFile(const std::string &s);
};

SPIFlash::
SPIFlash(RunnableQueue &scheduler, PortConnectionWrapper miso,
         PortConnectionWrapper mosi, PortConnectionWrapper sclk,
         PortConnectionWrapper ss) :
  mem(0),
  memSize(0),
  MISO(miso.getInterface()),
  SCLKProxy(*this, &SPIFlash::seeSCLKChange),
  SSProxy(*this, &SPIFlash::seeSSChange),
  SCLKHandleClock(scheduler, SCLKProxy),
  SSHandleClock(scheduler, SSProxy),
  MISOValue(0),
  SCLKValue(0),
  SSValue(0)
{
  mosi.attach(&MOSITracker);
  sclk.attach(&SCLKHandleClock);
  ss.attach(&SSHandleClock);
  reset();
}

SPIFlash::~SPIFlash()
{
  delete[] mem;
}

void SPIFlash::reset()
{
  state = WAIT_FOR_CMD;
  receiveReg = 0;
  receivedBits = 0;
  receivedAddressBytes = 0;
  readAddress = 0;
  sendReg = 0;
  sendBitsRemaining = 0;
}

void SPIFlash::seeSCLKChange(const Signal &value, ticks_t time)
{
  unsigned newValue = value.getValue(time);
  if (newValue == SCLKValue)
    return;
  SCLKValue = newValue;
  if (SSValue != 0)
    return;
  if (SCLKValue == 1) {
    // Rising edge.
    receiveReg = (receiveReg << 1) | MOSITracker.getSignal().getValue(time);
    if (++receivedBits == 8) {
      switch (state) {
      case WAIT_FOR_CMD:
        if (receiveReg == 0x3)
          state = WAIT_FOR_ADDRESS;
        else
          state = UNKNOWN_CMD;
        break;
      case WAIT_FOR_ADDRESS:
        readAddress = (readAddress << 8) | receiveReg;
        if (++receivedAddressBytes == 3) {
          state = READ;
        }
      case READ:
      case UNKNOWN_CMD:
        // Do nothing.
        break;
      }
      receiveReg = 0;
      receivedBits = 0;
    }
  } else {
    // Falling edge.
    if (state == READ) {
      // Output
      if (mem && sendBitsRemaining == 0) {
        sendReg = mem[readAddress++ % memSize];
        sendBitsRemaining = 8;
      }
      unsigned newValue = (sendReg >> 7) & 1;
      if (newValue != MISOValue) {
        MISO->seePinsChange(Signal(newValue), time);
        MISOValue = newValue;
      }
      sendReg <<= 1;
      --sendBitsRemaining;
    }
  }
}

void SPIFlash::seeSSChange(const Signal &value, ticks_t time)
{
  unsigned newValue = value.getValue(time);
  if (newValue == SSValue)
    return;
  SSValue = newValue;
  if (SSValue == 1)
    reset();
}

void SPIFlash::openFile(const std::string &s)
{
  delete[] mem;
  std::ifstream file(s.c_str(),
                     std::ios::in|std::ios::binary|std::ios::ate);
  if (!file) {
    std::cerr << "Error opening \"" << s << "\"\n";
    std::exit(1);
  }
  memSize = file.tellg();
  mem = new uint8_t[memSize];
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(mem), memSize);
  if (!file) {
    std::cerr << "Error reading \"" << s << "\"\n";
    std::exit(1);
  }
  file.close();
}

static Peripheral *
createSPIFlash(SystemState &system, PortConnectionManager &connectionManager,
               const Properties &properties)
{
  PortConnectionWrapper MISO = connectionManager.get(properties, "miso");
  PortConnectionWrapper MOSI = connectionManager.get(properties, "mosi");
  PortConnectionWrapper SCLK = connectionManager.get(properties, "sclk");
  PortConnectionWrapper SS = connectionManager.get(properties, "ss");
  std::string file = properties.get("filename")->getAsString();
  SPIFlash *p = new SPIFlash(system.getScheduler(), MISO, MOSI, SCLK, SS);
  p->openFile(file);
  return p;
}

std::unique_ptr<PeripheralDescriptor> axe::getPeripheralDescriptorSPIFlash()
{
  std::unique_ptr<PeripheralDescriptor> p(
    new PeripheralDescriptor("spi-flash", &createSPIFlash));
  p->addProperty(PropertyDescriptor::portProperty("miso")).setRequired(true);
  p->addProperty(PropertyDescriptor::portProperty("mosi")).setRequired(true);
  p->addProperty(PropertyDescriptor::portProperty("sclk")).setRequired(true);
  p->addProperty(PropertyDescriptor::portProperty("ss")).setRequired(true);
  p->addProperty(PropertyDescriptor::stringProperty("filename"))
    .setRequired(true);
  return p;
}
