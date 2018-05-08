#ifndef __WatchpointException_
#define __WatchpointException_

#include <stdint.h>
#include "Resource.h"
#include "WatchpointManager.h"

namespace axe {

class Thread;
class WatchpointException {
private:
  uint32_t address;
public:
  WatchpointType type;
  Thread &thread;
  ticks_t time;
  WatchpointException(WatchpointType t, uint32_t addr, Thread &thrd,
                      ticks_t tm) :
    address(addr), type(t), thread(thrd), time(tm) {}
  uint32_t getAddr() { return address; }
  ticks_t getTime() const { return time; }
  Thread &getThread() const { return thread; }
};

} // End axe namespace

#endif // __WatchpointException_
