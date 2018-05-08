// Copyright (c) 2011, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "Resource.h"
#include "ClockBlock.h"
#include "Core.h"
#include "PortNames.h"
#include <algorithm>

using axe::Port;
using axe::Signal;
using axe::Resource;

Port::Port() :
  EventableResource(RES_TYPE_PORT),
  data(0),
  condition(COND_FULL),
  clock(nullptr),
  readyOutOf(nullptr),
  portCounter(0),
  loopback(nullptr),
  tracer(nullptr),
  pausedOut(nullptr),
  pausedIn(nullptr),
  pausedSync(nullptr),
  shiftRegEntries(0),
  portShiftCount(0),
  validShiftRegEntries(0),
  shiftReg(0),
  transferReg(0),
  timeReg(0),
  readyOut(false),
  readyIn(false),
  transferWidth(0),
  timeRegValid(false),
  transferRegValid(false),
  timestampReg(0),
  holdTransferReg(false),
  time(0),
  outputPort(false),
  buffered(false),
  inverted(false),
  samplingEdge(Edge::RISING),
  readyMode(NOREADY),
  masterSlave(MASTER),
  portType(DATAPORT) {}

std::string Port::getName() const
{
  std::string name;
  if (getPortName(getID(), name)) {
    return name;
  }
  return "(Unknown port)";
}

Signal Port::getDataPortPinsValue() const
{
  assert(portType == DATAPORT);
  if (outputPort) {
    return Signal(shiftReg & portWidthMask());
  }
  return pinsInputValue;
}

Signal Port::getPinsValue() const {
  if (outputPort) {
    return getPinsOutputValue();
  }
  return pinsInputValue;
}

bool Port::setCInUse(Thread &thread, bool val, ticks_t newTime)
{
  // TODO call update()?
  if (val) {
    data = 0;
    condition = COND_FULL;
    outputPort = false;
    buffered = false;
    inverted = false;
    samplingEdge = Edge::RISING;
    transferRegValid = false;
    timeRegValid = false;
    holdTransferReg = false;
    validShiftRegEntries = 0;
    timestampReg = 0;
    shiftReg = 0;
    shiftRegEntries = 1;
    portShiftCount = 1;
    time = newTime;
    portCounter = 0;
    readyIn = false;
    readyMode = NOREADY;
    // TODO check.
    masterSlave = MASTER;
    portType = DATAPORT;
    transferWidth = getPortWidth();
    if (readyOutOf != nullptr) {
      readyOutOf->detachReadyOut(*this);
      readyOutOf = nullptr;
    }
    if (clock->isFixedFrequency()) {
      nextEdge = clock->getEdgeIterator(time);
    }
    clearReadyOut(time);
  }
  eventableSetInUse(thread, val);
  return true;
}

bool Port::
setCondition(Thread &thread, Condition c, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (c == COND_AFTER) {
    return false;
  }
  condition = c;
  scheduleUpdateIfNeeded();
  return true;
}

bool Port::
setData(Thread &thread, uint32_t d, ticks_t time)
{
  update(time);
  updateOwner(thread);
  data = d & portWidthMask();
  scheduleUpdateIfNeeded();
  return true;
}

bool Port::getData(Thread &thread, uint32_t &result, ticks_t time)
{
  update(time);
  updateOwner(thread);
  result = data;
  scheduleUpdateIfNeeded();
  return true;
}

bool Port::setPortInv(Thread &thread, bool value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (inverted == value) {
    return true;
  }
  if (value && getPortWidth() != 1) {
    return false;
  }
  inverted = value;
  // TODO inverting the port should only change the direction if we are actually
  // driving the pins.
  //if (outputPort || portType != DATAPORT) {
    outputValue(getPinsOutputValue(), time);
  //}
  return true;
}

void Port::setSamplingEdge(Thread &thread, Edge::Type value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (samplingEdge == value) {
    return;
  }
  samplingEdge = value;
  scheduleUpdateIfNeeded();
}

bool Port::setPinDelay(Thread &thread, unsigned value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  return value <= 5;
  // TODO set pin delay
}

Signal Port::getPinsOutputValue() const
{
  if (portType == READYPORT) {
    if (readyOutOf != nullptr) {
      return getEffectiveValue(Signal(readyOutOf->getReadyOutValue()));
    }
    return getEffectiveValue(Signal(0));
  }
  if (portType == CLOCKPORT) {
    return getEffectiveValue(clock->getValue());
  }
  assert(portType == DATAPORT);
  if (!outputPort) {
    return getEffectiveValue(Signal(0));
  }
  return getEffectiveValue(Signal(shiftReg & portWidthMask()));
}

void Port::
outputValue(Signal value, ticks_t time)
{
  if (loopback != nullptr) {
    loopback->seePinsChange(value, time);
  }
  if (outputPort) {
    handlePinsChange(value, time);
  }
}

void Port::
handlePinsChange(Signal value, ticks_t time)
{
  if (isInUse()) {
    Signal effectiveValue = getEffectiveValue(value);
    for (ClockBlock *clk : sourceOf) {
      clk->setValue(effectiveValue, time);
    }
    for (ClockBlock *clk : readyInOf) {
      clk->setReadyInValue(effectiveValue, time);
    }
  }
  if (tracer != nullptr) {
    tracer->seePinsChange(value, time);
  }
}

void Port::
handlePinsChange(uint32_t value, ticks_t time)
{
  handlePinsChange(Signal(value), time);
}

void Port::
handleReadyOutChange(bool value, ticks_t time)
{
  for (Port *port : readyOutPorts) {
    port->outputValue(getEffectiveValue(static_cast<uint32_t>(value)), time);
  }
}

void Port::
seePinsChange(const Signal &value, ticks_t time)
{
  update(time);
  pinsInputValue = value;
  if (isInUse() && outputPort) {
    return;
  }
  handlePinsChange(value, time);
  scheduleUpdateIfNeeded();
}

void Port::updateNoExternalChange(unsigned numEdges)
{
  if (numEdges == 0) {
    return;
  }
  
  bool slowMode = false;
  ticks_t newTime = (nextEdge + (numEdges - 1))->time;
  if (slowMode) {
    bool oldOutputPort = outputPort;
    Signal oldOutputValue = getPinsOutputValue();
    bool oldReadyOut = readyOut;
    updateSlow(newTime);
    assert(oldOutputPort == outputPort);
    assert(oldOutputValue == getPinsOutputValue());
    assert(oldReadyOut == readyOut);
    // Silence unused variable warnings.
    (void)oldOutputPort;
    (void)oldOutputValue;
    (void)oldReadyOut;
    return;
  }

  if (useReadyIn()) {
    if (clock->getReadyInValue().isClock()) {
      updateSlow(newTime);
      return;
    }
    while (static_cast<unsigned int>(readyIn) != clock->getReadyInValue(time)) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
    if (!readyIn) {
      if (timeRegValid && numEdges >= fallingEdgesUntilTimeMet()) {
        // TODO is behaviour right if !outputPort && !useReadyOut()?. Should
        // this be isBuffered (seeEdge would also need updating).
        if (outputPort || useReadyOut()) {
          timeRegValid = false;
          validShiftRegEntries = 0;
        }
      }
      updatePortCounter(numEdges);
      nextEdge += numEdges;
      time = newTime;
      return;
    }
  }

  if (outputPort) {
    while (validShiftRegEntries != 0 || portShiftCount != shiftRegEntries) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
    if (!timeRegValid) {
      updatePortCounter(numEdges);
      nextEdge += numEdges;
      time = newTime;
      return;
    }
    unsigned numFalling = (numEdges + static_cast<unsigned int>(nextEdge->type == Edge::FALLING)) / 2;
    unsigned fallingEdgesRemaining = fallingEdgesUntilTimeMet();
    if (numFalling < fallingEdgesRemaining) {
      portCounter += numFalling;
      nextEdge += numEdges;
      time = newTime;
      return;
    }
    unsigned edgesRemaining = edgesUntilTimeMet();
    portCounter += fallingEdgesRemaining - 1;
    nextEdge += edgesRemaining - 1;
    numEdges -= edgesRemaining - 1;
    while (timeRegValid) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
    while (validShiftRegEntries != 0 || portShiftCount != shiftRegEntries) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
    updatePortCounter(numEdges);
    nextEdge += numEdges;
    time = newTime;
    return;
  }

  if (pinsInputValue.isClock()) {
    updateSlow(newTime);
    return;
  }
  if (timeRegValid) {
    if (!useReadyOut()) {
      uint32_t steadyStateShiftReg = computeSteadyStateInputShiftReg();
      while (shiftReg != steadyStateShiftReg ||
             portShiftCount != shiftRegEntries) {
        seeEdge(nextEdge++);
        if (--numEdges == 0) {
          return;
        }
      }
    }
    unsigned numFalling = (numEdges + static_cast<unsigned int>(nextEdge->type == Edge::FALLING)) / 2;
    unsigned fallingEdgesRemaining = fallingEdgesUntilTimeMet();
    if (numFalling < fallingEdgesRemaining) {
      updatePortCounter(numEdges);
      nextEdge += numEdges;
      time = newTime;
      return;
    }
    unsigned edgesRemaining = edgesUntilTimeMet();
    updatePortCounter(edgesRemaining - 1);
    nextEdge += edgesRemaining - 1;
    numEdges -= edgesRemaining - 1;
    while (timeRegValid) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
  }

  if (useReadyOut()) {
    uint32_t steadyStateShiftReg = computeSteadyStateInputShiftReg();
    if (!valueMeetsCondition(getEffectiveDataPortInputPinsValue(time))) {
      while (shiftReg != steadyStateShiftReg ||
             portShiftCount != shiftRegEntries) {
        seeEdge(nextEdge++);
        if (--numEdges == 0) {
          return;
        }
      }
      updatePortCounter(numEdges);
      updateInputValidShiftRegEntries(numEdges);
      nextEdge += numEdges;
      time = newTime;
      return;
    }
    while (condition != COND_FULL) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
    while (!transferRegValid || portShiftCount != shiftRegEntries) {
      seeEdge(nextEdge++);
      if (--numEdges == 0) {
        return;
      }
    }
    updatePortCounter(numEdges);
    nextEdge += numEdges;
    time = newTime;
    return;
  }
  
  uint32_t steadyStateShiftReg = computeSteadyStateInputShiftReg();
  while (!transferRegValid || portShiftCount != shiftRegEntries ||
         shiftReg != steadyStateShiftReg ||
         transferReg != steadyStateShiftReg) {
    seeEdge(nextEdge++);
    if (--numEdges == 0) {
      return;
    }
  }
  updatePortCounter(numEdges);
  updateInputValidShiftRegEntries(numEdges);
  nextEdge += numEdges;
  time = newTime;
}

void Port::update(ticks_t newTime)
{
  // Note updating the current port may result in callbacks for other ports /
  // clocks being called. These callbacks may in turn call callbacks for the
  // current port. Because of this we must ensure the port time and nextEdge are
  // updated before any callback is called.
  assert(newTime >= time);
  if (!clock->isFixedFrequency() || !isInUse() || portType != DATAPORT) {
    time = newTime;
    return;
  }
  assert(nextEdge == clock->getValue().getEdgeIterator(time));
  // Don't try and optimize the case of 2 or less edges.
  if ((nextEdge + 2)->time > newTime) {
    updateSlow(newTime);
    return;
  }
  unsigned numEdges = clock->getValue().getEdgeIterator(newTime) - nextEdge;
  // Skip all but the last edge. There can be no externally visisble changes in
  // this time (otherwise the port would have been scheduled to run at en
  // earlier time).
  updateNoExternalChange(numEdges - 1);
  // Handle the last edge.
  seeEdge(nextEdge++);
  time = newTime;
}


uint32_t Port::computeSteadyStateInputShiftReg()
{
  Signal current = getEffectiveDataPortInputPinsValue();
  assert(!current.isClock());
  uint32_t val = current.getValue(time);
  unsigned width = shiftRegEntries;
  unsigned shift = getPortWidth();
  while (width > 1) {
    val = (val << shift) | val;
    width >>= 1;
    shift *= 2;
  }
  val &= makeMask(getTransferWidth());
  return val;
}

void Port::updateSlow(ticks_t newTime)
{
  while (nextEdge->time <= newTime) {
    seeEdge(nextEdge++);
  }
  time = newTime;
}

bool Port::
shouldRealignShiftRegister()
{
  assert(!outputPort);
  if (!isBuffered()) { return false; }
  if ((pausedIn == nullptr) && !eventsPermitted()) { return false; }
  if (holdTransferReg) { return false; }
  if (!valueMeetsCondition(getEffectiveDataPortInputPinsValue(time))) { return false; }
  if (timeRegValid) {
    return !useReadyOut() && portCounter == timeReg;
  }
  return condition != COND_FULL;
}

uint32_t Port::
nextShiftRegOutputPort(uint32_t old)
{
  uint32_t repeatValue = old >> (getTransferWidth() - getPortWidth());
  uint32_t retval = old >> getPortWidth();
  retval |= repeatValue << (getTransferWidth() - getPortWidth());
  return retval;
}

void Port::
seeFallingEdgeOutputPort()
{
  uint32_t nextShiftReg = shiftReg;
  bool nextOutputPort = outputPort;

  if (timeRegValid && timeReg == portCounter) {
    nextOutputPort = transferRegValid;
    timeRegValid = false;
    validShiftRegEntries = 0;
  }

  if (!useReadyIn() || readyIn) {

    if (validShiftRegEntries > 0) {
      validShiftRegEntries--;
    }

    if (validShiftRegEntries != 0) {
      nextShiftReg = nextShiftRegOutputPort(shiftReg);
    }

    if (validShiftRegEntries == 0) {
      if ((pausedSync != nullptr) && !transferRegValid) {
        pausedSync->time = time;
        pausedSync->pc++;
        pausedSync->schedule();
        pausedSync = nullptr;
      }
      if (!timeRegValid && transferRegValid) {
        validShiftRegEntries = portShiftCount;
        portShiftCount = shiftRegEntries;
        nextShiftReg = transferReg;
        timestampReg = portCounter;
        transferRegValid = false;

        if (pausedOut != nullptr) {
          pausedOut->time = time;
          pausedOut->schedule();
          pausedOut = nullptr;
        }
      } else if (!timeRegValid && (pausedIn != nullptr)) {
          nextOutputPort = false;
          validShiftRegEntries = 0;
      }
    }
  }

  bool pinsChange =
  ((shiftReg ^ (nextOutputPort ? nextShiftReg : 0)) & portWidthMask()) != 0u;

  shiftReg = nextShiftReg;
  outputPort = nextOutputPort;

  if (pinsChange) {
    uint32_t newValue = getPinsOutputValue(time);
    outputValue(newValue, time);
  }
}

void Port::
seeFallingEdge()
{
  portCounter++;
  if (outputPort) {
    seeFallingEdgeOutputPort();
  } else if (useReadyOut() && timeRegValid && portCounter == timeReg) {
    timeRegValid = false;
    validShiftRegEntries = 0;
  }
  updateReadyOut(time);
}

void Port::
seeSamplingEdge()
{
  if (outputPort) { return; }
  if (useReadyOut() && (!readyOut or timeRegValid)) { return; }
  if (useReadyIn() && !readyIn) { return; }

  uint32_t currentValue = getEffectiveDataPortInputPinsValue(time);
  shiftReg >>= getPortWidth();
  shiftReg |= currentValue << (getTransferWidth() - getPortWidth());
  validShiftRegEntries++;

  if (shouldRealignShiftRegister()) {
    validShiftRegEntries = portShiftCount;
    transferRegValid = false;
    timeRegValid = false;
    if (isBuffered()) {
      condition = COND_FULL;
    }
  } else if (isBuffered() && timeRegValid && !useReadyOut() &&
             portCounter == timeReg) {
    timeRegValid = false;
  }

  if (validShiftRegEntries == portShiftCount &&
      (!useReadyOut() || !transferRegValid ||
       timeRegValid || condition != COND_FULL)) {
    validShiftRegEntries = 0;

    if (!holdTransferReg) {
      portShiftCount = shiftRegEntries;
      //transferReg = shiftReg;
      timestampReg = portCounter;
      //transferRegValid = true;
      setTransferReg(shiftReg);

      if (timeAndConditionMet()) {
        // TODO should this be conditional on pausedIn || EventPermitted()?
        timeRegValid = false;

        if (pausedIn != nullptr) {
          pausedIn->time = time;
          pausedIn->schedule();
          pausedIn = nullptr;
        }

        if (eventsPermitted()) {
          event(time);
        }
      }
    }
  }
}

void Port::
seeEdge(Edge::Type edgeType, ticks_t newTime)
{
  assert(newTime >= time);
  time = newTime;
  if (portType != DATAPORT) {
    return;
  }
  if (edgeType == Edge::FALLING) {
    seeFallingEdge();
  }
  if (edgeType == samplingEdge) {
    readyIn = clock->getReadyInValue(time) != 0u;
    seeSamplingEdge();
  }
}

void Port::updatePortCounter(unsigned numEdges)
{
  unsigned numFalling = (numEdges + static_cast<unsigned int>(nextEdge->type == Edge::FALLING)) / 2;
  portCounter += numFalling;
}

void Port::updateInputValidShiftRegEntries(unsigned numEdges)
{
  assert(!outputPort && (!useReadyIn() || readyIn));
  unsigned numSampling = (numEdges + static_cast<unsigned int>(nextEdge->type == samplingEdge)) / 2;
  validShiftRegEntries = (validShiftRegEntries + numSampling) % shiftRegEntries;
}

bool Port::seeOwnerEventEnable()
{
  assert(eventsPermitted());
  if (timeAndConditionMet()) {
    event(getOwner().time);
    return true;
  }
  scheduleUpdateIfNeeded();
  return false;
}

void Port::seeClockStart(ticks_t time)
{
  if (!isInUse()) {
    return;
  }
  portCounter = 0;
  seeClockChange(time);
}

void Port::seeClockChange(ticks_t time)
{
  if (!isInUse()) {
    return;
  }
  if (portType == CLOCKPORT) {
    outputValue(getPinsOutputValue(), time);
  } else if (portType == DATAPORT && clock->isFixedFrequency()) {
    nextEdge = clock->getEdgeIterator(time);
  }
  scheduleUpdateIfNeeded();
}

bool Port::valueMeetsCondition(uint32_t value) const
{
  switch (condition) {
  default: assert(0 && "Unexpected condition");
  case COND_FULL:
    return true;
  case COND_EQ:
    return data == value;
  case COND_NEQ:
    return data != value;
  }
}

bool Port::isValidPortShiftCount(uint32_t count) const
{
  return count >= getPortWidth() && count <= getTransferWidth() &&
         (count % getPortWidth()) == 0;
}

Resource::ResOpResult Port::
in(Thread &thread, ticks_t threadTime, uint32_t &value)
{
  update(threadTime);
  updateOwner(thread);
  // TODO is this right?
  if (portType != DATAPORT) {
    value = 0;
    return CONTINUE;
  }
  if (outputPort) {
    pausedIn = &thread;
    scheduleUpdateIfNeeded();
    return DESCHEDULE;
  }
  if (timeAndConditionMet()) {
    value = transferReg;
    if (validShiftRegEntries == portShiftCount) {
      portShiftCount = shiftRegEntries;
      transferReg = shiftReg;
      validShiftRegEntries = 0;
      // TODO is this right?
      timestampReg = portCounter;
    } else {
      transferRegValid = false;
    }
    holdTransferReg = false;
    return CONTINUE;
  }
  pausedIn = &thread;
  scheduleUpdateIfNeeded();
  return DESCHEDULE;
}

Resource::ResOpResult Port::
inpw(Thread &thread, uint32_t width, ticks_t threadTime, uint32_t &value)
{
  update(threadTime);
  updateOwner(thread);
  if (!isBuffered() || !isValidPortShiftCount(width)) {
    return ILLEGAL;
  }
  // TODO is this right?
  if (portType != DATAPORT) {
    value = 0;
    return CONTINUE;
  }
  if (outputPort) {
    pausedIn = &thread;
    scheduleUpdateIfNeeded();
    return DESCHEDULE;
  }
  if (timeAndConditionMet()) {
    value = transferReg;
    // TODO should validShiftRegEntries be reset?
    //validShiftRegEntries = 0;
    if (validShiftRegEntries == portShiftCount) {
      portShiftCount = shiftRegEntries;
      transferReg = shiftReg;
      // TODO is this right?
      timestampReg = portCounter;
    } else {
      transferRegValid = false;
    }
    holdTransferReg = false;
    return CONTINUE;
  }
  portShiftCount = width / getPortWidth();
  pausedIn = &thread;
  scheduleUpdateIfNeeded();
  return DESCHEDULE;
}

Resource::ResOpResult Port::
out(Thread &thread, uint32_t value, ticks_t threadTime)
{
  update(threadTime);
  updateOwner(thread);
  // TODO is this right?
  if (portType != DATAPORT) {
    return CONTINUE;
  }
  if (outputPort) {
    if (transferRegValid) {
      pausedOut = &thread;
      scheduleUpdateIfNeeded();
      return DESCHEDULE;
    }
  } else {
    // TODO probably wrong.
    validShiftRegEntries = 1;
  }
  //transferRegValid = true;
  //transferReg = value;
  setTransferReg(value);
  outputPort = true;
  scheduleUpdateIfNeeded();
  return CONTINUE;
}

Resource::ResOpResult Port::
outpw(Thread &thread, uint32_t value, uint32_t width, ticks_t threadTime)
{
  update(threadTime);
  updateOwner(thread);
  if (!isBuffered() || !isValidPortShiftCount(width)) {
    return ILLEGAL;
  }
  // TODO is this right?
  if (portType != DATAPORT) {
    return CONTINUE;
  }
  if (outputPort) {
    if (transferRegValid) {
      pausedOut = &thread;
      scheduleUpdateIfNeeded();
      return DESCHEDULE;
    }
  } else {
    // TODO probably wrong.
    validShiftRegEntries = 1;
  }
  // transferRegValid = true;
  portShiftCount = width / getPortWidth();
  // transferReg = value;
  setTransferReg(value);
  outputPort = true;
  scheduleUpdateIfNeeded();
  return CONTINUE;
}

Port::ResOpResult Port::
setpsc(Thread &thread, uint32_t width, ticks_t threadTime)
{
  update(threadTime);
  updateOwner(thread);
  if (!isBuffered() || !isValidPortShiftCount(width)) {
    return ILLEGAL;
  }
  // TODO is this right?
  if (portType != DATAPORT) {
    return CONTINUE;
  }
  if (outputPort) {
    if (transferRegValid) {
      pausedOut = &thread;
      scheduleUpdateIfNeeded();
      return DESCHEDULE;
    }
  }
  portShiftCount = width / getPortWidth();
  scheduleUpdateIfNeeded();
  return CONTINUE;
}

Port::ResOpResult Port::
endin(Thread &thread, ticks_t threadTime, uint32_t &value)
{
  update(threadTime);
  updateOwner(thread);
  if (outputPort || !isBuffered()) {
    return ILLEGAL;
  }
  // TODO is this right?
  if (portType != DATAPORT) {
    value = 0;
    return CONTINUE;
  }
  unsigned entries = validShiftRegEntries;
  if (transferRegValid) {
    entries += shiftRegEntries;
    if (validShiftRegEntries != 0) {
      portShiftCount = validShiftRegEntries;
    }
  } else {
    validShiftRegEntries = 0;
    portShiftCount = shiftRegEntries;
    // transferReg = shiftReg;
    timestampReg = portCounter;
    // transferRegValid = true;
    setTransferReg(shiftReg);
  }
  value = entries * getPortWidth();
  scheduleUpdateIfNeeded();
  return CONTINUE;
}

Resource::ResOpResult Port::
sync(Thread &thread, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (portType != DATAPORT || !outputPort) {
    return CONTINUE;
  }
  pausedSync = &thread;
  scheduleUpdateIfNeeded();
  return DESCHEDULE;
}

uint32_t Port::
peek(Thread &thread, ticks_t threadTime)
{
  update(threadTime);
  updateOwner(thread);
  return getEffectiveInputPinsValue().getValue(threadTime);
}

uint32_t Port::
getTimestamp(Thread &thread, ticks_t time)
{
  update(time);
  updateOwner(thread);
  return timestampReg;
}

Resource::ResOpResult
Port::setPortTime(Thread &thread, uint32_t value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (portType != DATAPORT) {
    return CONTINUE;
  }
  if (outputPort) {
    if (transferRegValid) {
      pausedOut = &thread;
      scheduleUpdateIfNeeded();
      return DESCHEDULE;
    }
  }
  timeReg = value;
  timeRegValid = true;
  return CONTINUE;
}

void Port::clearPortTime(Thread &thread, ticks_t time)
{
  update(time);
  updateOwner(thread);
  timeRegValid = false;
}

void Port::clearBuf(Thread &thread, ticks_t time)
{
  update(time);
  updateOwner(thread);
  transferRegValid = false;
  holdTransferReg = false;
  validShiftRegEntries = 0;
  clearReadyOut(time);
}

bool Port::checkTransferWidth(uint32_t value)
{
  unsigned portWidth = getPortWidth();
  if (transferWidth == portWidth) {
    return true;
  }
  if (transferWidth < portWidth) {
    return false;
  }
  switch (value) {
  default: return false;
  // TODO check
  case 8:
  case 32:
    return true;
  }
}

Signal Port::getEffectiveValue(Signal value) const
{
  if (inverted) {
    value.flipLeastSignificantBit();
  }
  return value;
}

void Port::setClkInitial(ClockBlock *c) {
  clock = c;
  clock->attachPort(this);
  portCounter = 0;
  seeClockChange(time);
}

void Port::setClk(Thread &thread, ClockBlock *c, ticks_t time)
{
  update(time);
  updateOwner(thread);
  clock->detachPort(this);
  clock = c;
  clock->attachPort(this);
  portCounter = 0;
  seeClockChange(time);
}

bool Port::setReady(Thread &thread, Port *p, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (getPortWidth() != 1) {
    return false;
  }
  if (readyOutOf != nullptr) {
    p->detachReadyOut(*this);
  }
  readyOutOf = p;
  p->attachReadyOut(*this);
  outputValue(getEffectiveValue(p->getReadyOutValue()), time);
  return true;
}

bool Port::setBuffered(Thread &thread, bool value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (!value && (transferWidth != getPortWidth() || readyMode != NOREADY)) {
    return false;
  }
  buffered = value;
  return true;
}

bool Port::setReadyMode(Thread &thread, ReadyMode mode, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (mode != NOREADY && !buffered) {
    return false;
  }
  readyMode = mode;
  scheduleUpdateIfNeeded();
  return true;
}

bool Port::setMasterSlave(Thread &thread, MasterSlave value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  masterSlave = value;
  scheduleUpdateIfNeeded();
  return true;
}

bool Port::setPortType(Thread &thread, PortType type, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (portType == type) {
    return true;
  }
  Signal oldValue = getPinsOutputValue();
  bool oldOutputPort = outputPort;
  portType = type;
  if (type == DATAPORT) {
    if (clock->isFixedFrequency()) {
      nextEdge = clock->getEdgeIterator(time);
    }
  } else {
    outputPort = true;
  }
  Signal newValue = getPinsOutputValue();
  if (newValue != oldValue || !oldOutputPort) {
    outputValue(newValue, time);
  }
  scheduleUpdateIfNeeded();
  return true;
}

bool Port::setTransferWidth(Thread &thread, uint32_t value, ticks_t time)
{
  update(time);
  updateOwner(thread);
  if (!checkTransferWidth(value)) {
    return false;
  }
  transferWidth = value;
  shiftRegEntries = transferWidth / getPortWidth();
  portShiftCount = shiftRegEntries;
  return true;
}

unsigned Port::fallingEdgesUntilTimeMet() const
{
  assert(timeRegValid);
  return static_cast<uint16_t>(timeReg - (portCounter + 1)) + 1;
}

unsigned Port::edgesUntilTimeMet() const
{
  unsigned numFallingEdges = fallingEdgesUntilTimeMet();
  if (nextEdge->type == Edge::FALLING) {
    return numFallingEdges * 2 - 1;
  }
  return numFallingEdges * 2;
}

void Port::scheduleUpdateIfNeededOutputPort()
{
  // If the next edge is a falling edge unconditionally schedule an update.
  // This simplifies the rest of the code since we can assume the next edge
  // is a rising edge.
  if (nextEdge->type == Edge::FALLING) {
    return scheduleUpdate(nextEdge->time);
  }
  if (!readyOutIsInSteadyState()) {
    return scheduleUpdate((nextEdge + 1)->time);
  }
  bool readyInKnownZero = useReadyIn() && clock->getReadyInValue() == Signal(0);
  if (!readyInKnownZero) {
    if (nextShiftRegOutputPort(shiftReg) != shiftReg) {
      return scheduleUpdate((nextEdge + 1)->time);
    }
    if (useReadyOut() && readyOut) {
      return scheduleUpdate((nextEdge + 1)->time);
    }
  }
  if (timeRegValid) {
    // Port counter is always updated on the falling edge.
    unsigned fallingEdges = fallingEdgesUntilTimeMet();
    unsigned edges = 2 * fallingEdges - 1;
    return scheduleUpdate((nextEdge + edges)->time);
  }
  if (!readyInKnownZero &&
      ((pausedIn != nullptr) || (pausedSync != nullptr) || transferRegValid)) {
    return scheduleUpdate((nextEdge + 1)->time);
  }
}

void Port::scheduleUpdateIfNeededInputPort()
{
  // If the next edge is a rising edge unconditionally schedule an update.
  // This simplifies the rest of the code since we can assume the next edge
  // is a falling edge.
  if (nextEdge->type == Edge::RISING) {
    scheduleUpdate(nextEdge->time);

  // Next edge is falling edge
  } else if (!readyOutIsInSteadyState()) {
    scheduleUpdate(nextEdge->time);
  } else if (pausedOut != nullptr && !timeRegValid) {
    scheduleUpdate(nextEdge->time);
  } else if (timeRegValid) {
    unsigned fallingEdges = fallingEdgesUntilTimeMet();
    unsigned edges = (fallingEdges - 1) * 2;
    if (!useReadyOut() && samplingEdge == Edge::RISING) {
      edges++;
    }
    scheduleUpdate((nextEdge + edges)->time);
  } else if ((!useReadyIn() || clock->getReadyInValue() != Signal(0)) &&
      ((pausedIn != nullptr) || eventsPermitted() || (useReadyOut() && readyOut))) {
    Signal inputSignal = getEffectiveDataPortInputPinsValue();

    if (inputSignal.isClock() ||
        valueMeetsCondition(inputSignal.getValue(time))) {
      EdgeIterator nextSamplingEdge = nextEdge;

      if (nextSamplingEdge->type != samplingEdge) {
        ++nextSamplingEdge;
      }
      scheduleUpdate(nextSamplingEdge->time);
    }
  }
}

void Port::scheduleUpdateIfNeeded()
{
  if (!isInUse() || !clock->isFixedFrequency() || portType != DATAPORT) {
    return;
  }
  const bool slowMode = false;
  if (slowMode) {
    if ((pausedIn != nullptr) || eventsPermitted() || 
        (pausedOut != nullptr) || (pausedSync != nullptr) ||
        !sourceOf.empty() || useReadyOut() || (loopback != nullptr)) {
      return scheduleUpdate(nextEdge->time);
    }
  }
  if (outputPort) {
    scheduleUpdateIfNeededOutputPort();
  } else {
    scheduleUpdateIfNeededInputPort();
  }
}

bool Port::computeReadyOut()
{
  if (!useReadyOut()) {
    return false;
  }
  if (outputPort) {
    if (useReadyIn() && !readyIn) {
      return false;
    }
    return validShiftRegEntries != 0;
  }
  if (timeRegValid) {
    return portCounter == timeReg;
  }
  return validShiftRegEntries != portShiftCount;
}

/// Will the ready out signal change if port is clocked without there being any
/// external changes. Excludes changes in ready out signal due to the port time
/// or condition being met.
bool Port::readyOutIsInSteadyStateSlowPath()
{
  assert(useReadyOut());
  if (readyOut != computeReadyOut()) {
    return false;
  }

  if (outputPort && readyOut) {
    return false;
  }

  if (outputPort && validShiftRegEntries == 0) {
      return true;
  }

  if (outputPort) {
    assert(useReadyIn() && !readyIn);
    return clock->getReadyInValue() == Signal(0);
  }

  if (!readyOut) {
    return true;
  }

  if (timeRegValid) {
    return false;
  }

  if (useReadyIn() && !readyIn && clock->getReadyInValue() == Signal(0)) {
    return false;
  }

  if (readyOut && condition != COND_FULL &&
      !valueMeetsCondition(getEffectiveDataPortInputPinsValue(time))) {
    return true;
  }

  return false;
}

void Port::clearReadyOut(ticks_t time)
{
  if (!readyOut) {
    return;
  }
  readyOut = false;
  handleReadyOutChange(false, time);
}

void Port::updateReadyOut(ticks_t time)
{
  bool newValue = computeReadyOut();
  if (newValue == readyOut) {
    return;
  }
  readyOut = newValue;
  handleReadyOutChange(newValue, time);
}

void Port::completeEvent()
{
  assert(transferRegValid);
  holdTransferReg = true;
  EventableResource::completeEvent();
}

bool Port::seeEventEnable(ticks_t time)
{
  // TODO what about other ports?
  assert(portType == DATAPORT);
  if (timeAndConditionMet()) {
    event(time);
    return true;
  }
  scheduleUpdateIfNeeded();
  return false;
}
