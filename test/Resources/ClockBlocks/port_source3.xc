// RUN: xcc -O2 -target=XK-1A %s -o %t1.xe
// RUN: %sim %t1.xe

#include <xs1.h>

out port clk_out = XS1_PORT_1A;
out port p = XS1_PORT_1B;
clock c = XS1_CLKBLK_1;

#define NUM_FALLING 10
#define DELAY 50
// One output will be queued in advance
#define EXPECTED (DELAY * (2 * (NUM_FALLING - 1) - 1))
#define TOLERANCE 20

int main() {
  timer t;
  unsigned t1, t2;
  unsigned diff;
  clk_out <: 1;
  configure_clock_src(c, clk_out);
  configure_out_port(p, c, 0);
  start_clock(c);
  t :> t1;
  par {
    {
      unsigned time = t1;
      unsigned value = 0;
      timer t;
      for (unsigned i = 0; i < (NUM_FALLING * 2); i++) {
        t when timerafter(time+= DELAY) :> void;
        clk_out <: value;
        value = !value;
      }
    }
    {
      for (unsigned i = 0; i < NUM_FALLING; i++) {
        p <: 0;
      }
      t :> t2;
    }
  }
  diff = t2 - t1;
  if (diff < EXPECTED - TOLERANCE ||
      diff > EXPECTED + TOLERANCE)
    return 1;
  return 0;
}
